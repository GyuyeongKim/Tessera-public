#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tessera_c.h"

enum { C_X0, C_X1, C_S, C_TBL, C_K, C_X, C_V, C_MODE, C_MIN, C_THREADS };

typedef struct {
  sqlite3_vtab base;
  sqlite3 *db;
} TVtab;

typedef struct {
  sqlite3_vtab_cursor base;
  tessera_query_c *q;
  tessera_dict_c *dk, *dx, *dy;
  int eof, general, real;
  int int_v;
  int any_class;
  int cross;
  int sim_measure;
  double min_sim;
  int64_t x0, x1, vi, rowid;
  double vd;
} TCur;

static void free_query(TCur *c) {
  if (c->q) tessera_c_free(c->q);
  if (c->dk) tessera_dict_free(c->dk);
  if (c->dx) tessera_dict_free(c->dx);
  if (c->dy) tessera_dict_free(c->dy);
  c->q = 0;
  c->dk = c->dx = c->dy = 0;
}

static int interrupted(void *db) { return sqlite3_is_interrupted((sqlite3 *)db); }

static int key_of(sqlite3_stmt *st, int i, int *kind, tessera_dict_c **d, int64_t *out, int text_ok) {
  const int t = sqlite3_column_type(st, i);
  const int k = t == SQLITE_INTEGER ? 1 : t == SQLITE_TEXT ? 2 : 0;
  if (!k || (*kind && *kind != k)) return 1;
  if (k == 2 && !text_ok) return 5;
  *kind = k;
  if (k == 1) {
    *out = sqlite3_column_int64(st, i);
    return 0;
  }
  if (!*d && !(*d = tessera_dict_new())) return 2;
  const unsigned char *s = sqlite3_column_text(st, i);
  *out = tessera_dict_code(*d, (const char *)s, (size_t)sqlite3_column_bytes(st, i));
  return *out < 0 ? 2 : 0;
}

static int tConnect(sqlite3 *db, void *aux, int argc, const char *const *argv, sqlite3_vtab **pp, char **err) {
  int rc = sqlite3_declare_vtab(db,
      "CREATE TABLE x(x0, x1, s, tbl HIDDEN, kcol HIDDEN, xcol HIDDEN, vcol HIDDEN, mode HIDDEN, "
      "minval HIDDEN, threads HIDDEN)");
  if (rc != SQLITE_OK) return rc;
#ifdef SQLITE_VTAB_DIRECTONLY
  sqlite3_vtab_config(db, SQLITE_VTAB_DIRECTONLY);
#endif
  TVtab *v = sqlite3_malloc(sizeof *v);
  if (!v) return SQLITE_NOMEM;
  memset(v, 0, sizeof *v);
  v->db = db;
  *pp = &v->base;
  return SQLITE_OK;
}

static int tDisconnect(sqlite3_vtab *p) {
  sqlite3_free(p);
  return SQLITE_OK;
}

static int tOpen(sqlite3_vtab *p, sqlite3_vtab_cursor **pp) {
  TCur *c = sqlite3_malloc(sizeof *c);
  if (!c) return SQLITE_NOMEM;
  memset(c, 0, sizeof *c);
  *pp = &c->base;
  return SQLITE_OK;
}

static int tClose(sqlite3_vtab_cursor *cur) {
  TCur *c = (TCur *)cur;
  free_query(c);
  sqlite3_free(c);
  return SQLITE_OK;
}

static int best_index(sqlite3_vtab *tab, sqlite3_index_info *info, int first, int last, int required, const char *usage) {
  int at[32];
  for (int i = 0; i < 32; i++) at[i] = -1;
  for (int i = 0; i < info->nConstraint; i++) {
    const struct sqlite3_index_constraint *c = &info->aConstraint[i];
    if (c->iColumn >= first && c->iColumn <= last && c->op == SQLITE_INDEX_CONSTRAINT_EQ) {
      if (!c->usable) return SQLITE_CONSTRAINT;
      at[c->iColumn] = i;
    }
  }
  for (int col = first; col < first + required; col++)
    if (at[col] < 0) {
      sqlite3_free(tab->zErrMsg);
      tab->zErrMsg = sqlite3_mprintf("%s", usage);
      return SQLITE_ERROR;
    }
  int n = 0, mask = 0;
  for (int col = first; col <= last; col++) {
    if (at[col] < 0) continue;
    info->aConstraintUsage[at[col]].argvIndex = ++n;
    info->aConstraintUsage[at[col]].omit = 1;
    mask |= 1 << (col - first);
  }
  info->idxNum = mask;
  info->estimatedCost = 1e6;
  return SQLITE_OK;
}

static int tBestIndex(sqlite3_vtab *tab, sqlite3_index_info *info) {
  return best_index(tab, info, C_TBL, C_THREADS, 3,
                    "tessera_xtx(table, k column, x column [, v column, mode, min value, threads])");
}

static int fail(TCur *c, const char *msg) {
  sqlite3_vtab *tab = c->base.pVtab;
  sqlite3_free(tab->zErrMsg);
  tab->zErrMsg = sqlite3_mprintf("%s", msg);
  return SQLITE_ERROR;
}

static int fail_rc(TCur *c, const char *msg, int rc) {
  fail(c, msg);
  return rc;
}

static int db_utf8(sqlite3 *db) {
  sqlite3_stmt *st = 0;
  int utf8 = 0;
  if (sqlite3_prepare_v2(db, "PRAGMA encoding", -1, &st, 0) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
    const unsigned char *e = sqlite3_column_text(st, 0);
    utf8 = e && sqlite3_stricmp((const char *)e, "UTF-8") == 0;
  }
  sqlite3_finalize(st);
  return utf8;
}

static int tNext(sqlite3_vtab_cursor *cur) {
  TCur *c = (TCur *)cur;
  c->eof = !tessera_c_next(c->q, &c->x0, &c->x1, &c->vi, &c->vd, &c->general);
  c->rowid++;
  return SQLITE_OK;
}

#define LOAD_REAL 1000

static int load_rows(TCur *c, sqlite3 *db, const char *fn, const char *tbl, const char *kc, const char *xc, const char *vc,
                     int side, int *kkind, int *xkind) {
  const char *cols[2] = {kc, xc};
  int text_ok[2];
  for (int i = 0; i < 2; i++) {
    const char *coll = 0;
    text_ok[i] = sqlite3_table_column_metadata != 0 &&
                 sqlite3_table_column_metadata(db, 0, tbl, cols[i], 0, &coll, 0, 0, 0) == SQLITE_OK;
    if (text_ok[i] && coll && sqlite3_stricmp(coll, "BINARY") != 0) {
      char msg[200];
      snprintf(msg, sizeof msg, "%s: column %s has collation %s; only BINARY is supported", fn, cols[i], coll);
      return fail(c, msg);
    }
  }
  char *sql = vc ? sqlite3_mprintf("SELECT \"%w\", \"%w\", \"%w\" FROM \"%w\"", kc, xc, vc, tbl)
                 : sqlite3_mprintf("SELECT \"%w\", \"%w\" FROM \"%w\"", kc, xc, tbl);
  sqlite3_stmt *st = 0;
  int rc = sqlite3_prepare_v2(db, sql, -1, &st, 0);
  sqlite3_free(sql);
  if (rc != SQLITE_OK) return fail_rc(c, sqlite3_errmsg(db), rc);
  enum { BATCH = 65536 };
  int64_t *kb = malloc(sizeof(int64_t) * BATCH), *xb = malloc(sizeof(int64_t) * BATCH);
  const int iv = vc && c->int_v;
  double *vb = vc && !iv ? malloc(sizeof(double) * BATCH) : 0;
  int64_t *vib = iv ? malloc(sizeof(int64_t) * BATCH) : 0;
  tessera_dict_c **dxs = side ? &c->dy : &c->dx;
  int n = 0, bad = 0;
  if (!kb || !xb || (vc && !vb && !vib)) bad = 2;
#define ADD_BATCH()                                                                                        \
  (iv ? (side ? tessera_c_add_y_i64(c->q, kb, xb, vib, n) : tessera_c_add_i64(c->q, kb, xb, vib, n)) \
      : (side ? tessera_c_add_y(c->q, kb, xb, vb, n) : tessera_c_add(c->q, kb, xb, vb, n)))
  while (!bad && (rc = sqlite3_step(st)) == SQLITE_ROW) {
    if (sqlite3_column_type(st, 0) == SQLITE_NULL) continue;
    if (sqlite3_column_type(st, 1) == SQLITE_NULL || (vc && sqlite3_column_type(st, 2) == SQLITE_NULL)) { bad = 1; break; }
    const int e = key_of(st, 0, kkind, &c->dk, &kb[n], text_ok[0]);
    const int f = e ? e : key_of(st, 1, xkind, dxs, &xb[n], text_ok[1]);
    if (e || f) { const int g = e ? e : f; bad = g == 2 ? 2 : g == 5 ? (e ? 5 : 6) : 3; break; }
    const int vt = vc ? sqlite3_column_type(st, 2) : 0;
    if (vc && vt != SQLITE_INTEGER && vt != SQLITE_FLOAT) { bad = 7; break; }
    if (iv) {
      if (vt != SQLITE_INTEGER) { bad = 4; break; }
      vib[n] = sqlite3_column_int64(st, 2);
    } else if (vc) {
      if (vt == SQLITE_INTEGER && !c->any_class) { bad = 8; break; }
      vb[n] = sqlite3_column_double(st, 2);
      if (vt == SQLITE_FLOAT) c->real = 1;
    }
    if (++n == BATCH) {
      if (ADD_BATCH() != 0) bad = 2;
      n = 0;
    }
  }
  if (!bad && n && ADD_BATCH() != 0) bad = 2;
#undef ADD_BATCH
  sqlite3_finalize(st);
  free(kb);
  free(xb);
  free(vb);
  free(vib);
  if (bad == 4) return LOAD_REAL;
  char msg[300];
  if (bad == 1) { snprintf(msg, sizeof msg, "%s: NULL group keys or values are not supported", fn); return fail(c, msg); }
  if (bad == 2) { snprintf(msg, sizeof msg, "%s: out of memory", fn); return fail_rc(c, msg, SQLITE_NOMEM); }
  if (bad == 3) { snprintf(msg, sizeof msg, "%s: keys must be all INTEGER or all TEXT per column", fn); return fail(c, msg); }
  if (bad == 5 || bad == 6) {
    snprintf(msg, sizeof msg, "%s: the collation of TEXT column %s of %s cannot be read (a view, or SQLite without "
             "SQLITE_ENABLE_COLUMN_METADATA); TEXT keys need a table", fn, bad == 5 ? kc : xc, tbl);
    return fail(c, msg);
  }
  if (bad == 7) {
    snprintf(msg, sizeof msg, "%s: column %s holds TEXT or BLOB values; CAST it to INTEGER or REAL in a view", fn, vc);
    return fail(c, msg);
  }
  if (bad == 8) {
    snprintf(msg, sizeof msg, "%s: column %s mixes INTEGER and REAL values, which SQLite sums with a type per group; "
             "CAST it to one of them in a view", fn, vc);
    return fail(c, msg);
  }
  if (rc != SQLITE_DONE && rc != SQLITE_ROW) return fail_rc(c, sqlite3_errmsg(db), rc);
  return 0;
}

static int run_query_c(TCur *c, sqlite3 *db, int mode, int64_t minval, int threads) {
  if (c->sim_measure) tessera_c_set_similarity(c->q, c->sim_measure, c->min_sim);
  if (c->dx) tessera_c_set_xdict(c->q, c->dx);
  if (c->dy) tessera_c_set_ydict(c->q, c->dy);
  const sqlite3_int64 limit = sqlite3_soft_heap_limit64(-1);
  const char *tmp = getenv("SQLITE_TMPDIR");
  tessera_c_set_memory(c->q, limit > 0 ? (size_t)limit : 0, tmp && *tmp ? tmp : 0);
  tessera_c_set_stop(c->q, interrupted, db);
  char err[256];
  const int r = tessera_c_run(c->q, mode, 1, (uint64_t)(minval > 0 ? minval : 0), -1, threads, -1,
                              getenv("TESSERA_VERBOSE") != 0, err, sizeof err);
  if (r == TESSERA_C_CANCELLED) return fail_rc(c, "interrupted", SQLITE_INTERRUPT);
  if (r == TESSERA_C_OUT_OF_MEMORY) return fail_rc(c, err, SQLITE_NOMEM);
  if (r == TESSERA_C_SPILL_LIMIT) return fail_rc(c, err, SQLITE_FULL);
  if (r == TESSERA_C_SPILL_ERROR) return fail_rc(c, err, SQLITE_IOERR);
  if (r != TESSERA_C_OK) return fail(c, err);
  if (c->int_v) {
    const int ov = tessera_c_overflow(c->q, 64, 64);
    if (ov & 1) return fail(c, "tessera: a product a.v * b.v passes 64 bits, where SQLite computes it as REAL; run "
                               "the query in SQL");
    if (ov & 2) return fail(c, "integer overflow");
  }
  c->rowid = 0;
  return tNext(&c->base);
}

static int tFilter(sqlite3_vtab_cursor *cur, int mask, const char *idxStr, int argc, sqlite3_value **argv) {
  TCur *c = (TCur *)cur;
  sqlite3 *db = ((TVtab *)cur->pVtab)->db;
  const char *arg[C_THREADS + 1] = {0};
  sqlite3_value *val[C_THREADS + 1] = {0};
  for (int col = C_TBL, a = 0; col <= C_THREADS; col++)
    if (mask & (1 << (col - C_TBL))) {
      val[col] = argv[a++];
      arg[col] = (const char *)sqlite3_value_text(val[col]);
    }
  if (!tessera_c_supported()) return fail(c, "tessera_xtx: this CPU lacks x86-64-v3 (AVX2, BMI2, FMA), which Tessera needs");
  if (!arg[C_TBL] || !arg[C_K] || !arg[C_X]) return fail(c, "tessera_xtx: the table, k and x arguments must not be NULL");
  const int has_v = val[C_V] && sqlite3_value_type(val[C_V]) != SQLITE_NULL;
  int mode = 0;
  if (arg[C_MODE]) {
    if (strcmp(arg[C_MODE], "lt") == 0) mode = 1;
    else if (strcmp(arg[C_MODE], "le") == 0) mode = 2;
    else if (strcmp(arg[C_MODE], "full") != 0) return fail(c, "tessera_xtx: mode is 'full', 'lt' or 'le'");
  }
  const int64_t minval = val[C_MIN] && sqlite3_value_type(val[C_MIN]) != SQLITE_NULL ? sqlite3_value_int64(val[C_MIN]) : 0;
  const int threads = val[C_THREADS] && sqlite3_value_type(val[C_THREADS]) != SQLITE_NULL ? sqlite3_value_int(val[C_THREADS]) : 0;
  for (int int_v = has_v;; int_v = 0) {
    free_query(c);
    c->q = tessera_c_new(has_v);
    c->real = 0;
    c->int_v = int_v;
    c->cross = 0;
    c->sim_measure = 0;
    c->any_class = 0;
    if (!c->q) return fail_rc(c, "tessera_xtx: out of memory", SQLITE_NOMEM);
    int kkind = 0, xkind = 0;
    const int r = load_rows(c, db, "tessera_xtx", arg[C_TBL], arg[C_K], arg[C_X], has_v ? arg[C_V] : 0, 0, &kkind, &xkind);
    if (r == LOAD_REAL) continue;
    if (r) return r;
    if (mode && xkind == 2 && !db_utf8(db))
      return fail(c, "tessera_xtx: mode 'lt' or 'le' on TEXT group keys needs a UTF-8 database (BINARY orders UTF-16 "
                     "text by its UTF-16 bytes)");
    break;
  }
  return run_query_c(c, db, mode, minval, threads);
}

static int tEof(sqlite3_vtab_cursor *cur) { return ((TCur *)cur)->eof; }

static int tColumn(sqlite3_vtab_cursor *cur, sqlite3_context *ctx, int col) {
  TCur *c = (TCur *)cur;
  switch (col) {
    case C_X0:
    case C_X1: {
      const int64_t x = col == C_X0 ? c->x0 : c->x1;
      size_t len;
      const char *s = c->cross && col == C_X1 ? (c->dy ? tessera_c_ystr(c->q, x, &len) : 0)
                                              : (c->dx ? tessera_c_xstr(c->q, x, &len) : 0);
      if (s) sqlite3_result_text(ctx, s, (int)len, SQLITE_TRANSIENT);
      else sqlite3_result_int64(ctx, x);
      break;
    }
    case C_S:
      if (c->general) sqlite3_result_double(ctx, c->vd);
      else if (c->real) sqlite3_result_double(ctx, (double)c->vi);
      else sqlite3_result_int64(ctx, c->vi);
      break;
    default: sqlite3_result_null(ctx);
  }
  return SQLITE_OK;
}

static int tRowid(sqlite3_vtab_cursor *cur, sqlite_int64 *r) {
  *r = ((TCur *)cur)->rowid;
  return SQLITE_OK;
}

static sqlite3_module tesseraModule = {
    .iVersion = 0,
    .xCreate = 0,
    .xConnect = tConnect,
    .xBestIndex = tBestIndex,
    .xDisconnect = tDisconnect,
    .xOpen = tOpen,
    .xClose = tClose,
    .xFilter = tFilter,
    .xNext = tNext,
    .xEof = tEof,
    .xColumn = tColumn,
    .xRowid = tRowid,
};

enum { Y_X, Y_Y, Y_S, Y_T1, Y_K1, Y_X1, Y_T2, Y_K2, Y_Y2, Y_V1, Y_V2, Y_MIN, Y_THREADS };

static int yConnect(sqlite3 *db, void *aux, int argc, const char *const *argv, sqlite3_vtab **pp, char **err) {
  int rc = sqlite3_declare_vtab(db,
      "CREATE TABLE x(x, y, s, t1 HIDDEN, k1 HIDDEN, xcol HIDDEN, t2 HIDDEN, k2 HIDDEN, ycol HIDDEN, vcol HIDDEN, "
      "wcol HIDDEN, minval HIDDEN, threads HIDDEN)");
  if (rc != SQLITE_OK) return rc;
#ifdef SQLITE_VTAB_DIRECTONLY
  sqlite3_vtab_config(db, SQLITE_VTAB_DIRECTONLY);
#endif
  TVtab *v = sqlite3_malloc(sizeof *v);
  if (!v) return SQLITE_NOMEM;
  memset(v, 0, sizeof *v);
  v->db = db;
  *pp = &v->base;
  return SQLITE_OK;
}

static int yBestIndex(sqlite3_vtab *tab, sqlite3_index_info *info) {
  return best_index(tab, info, Y_T1, Y_THREADS, 6,
                    "tessera_xty(table 1, k column 1, x column, table 2, k column 2, y column [, v column, w column, "
                    "min value, threads])");
}

static int yFilter(sqlite3_vtab_cursor *cur, int mask, const char *idxStr, int argc, sqlite3_value **argv) {
  TCur *c = (TCur *)cur;
  sqlite3 *db = ((TVtab *)cur->pVtab)->db;
  const char *arg[Y_THREADS + 1] = {0};
  sqlite3_value *val[Y_THREADS + 1] = {0};
  for (int col = Y_T1, a = 0; col <= Y_THREADS; col++)
    if (mask & (1 << (col - Y_T1))) {
      val[col] = argv[a++];
      arg[col] = (const char *)sqlite3_value_text(val[col]);
    }
  if (!tessera_c_supported()) return fail(c, "tessera_xty: this CPU lacks x86-64-v3 (AVX2, BMI2, FMA), which Tessera needs");
  if (!arg[Y_T1] || !arg[Y_K1] || !arg[Y_X1] || !arg[Y_T2] || !arg[Y_K2] || !arg[Y_Y2])
    return fail(c, "tessera_xty: the table, k and x / y arguments must not be NULL");
  const int has_v = val[Y_V1] && sqlite3_value_type(val[Y_V1]) != SQLITE_NULL;
  const int has_w = val[Y_V2] && sqlite3_value_type(val[Y_V2]) != SQLITE_NULL;
  if (has_v != has_w) return fail(c, "tessera_xty: give both value columns (SUM(a.v * b.w)) or neither (COUNT(*))");
  const int64_t minval = val[Y_MIN] && sqlite3_value_type(val[Y_MIN]) != SQLITE_NULL ? sqlite3_value_int64(val[Y_MIN]) : 0;
  const int threads = val[Y_THREADS] && sqlite3_value_type(val[Y_THREADS]) != SQLITE_NULL ? sqlite3_value_int(val[Y_THREADS]) : 0;
  for (int int_v = has_v;; int_v = 0) {
    free_query(c);
    c->q = tessera_c_new_cross(has_v);
    c->real = 0;
    c->int_v = int_v;
    c->cross = 1;
    c->sim_measure = 0;
    c->any_class = 0;
    if (!c->q) return fail_rc(c, "tessera_xty: out of memory", SQLITE_NOMEM);
    int kkind = 0, xkind = 0, ykind = 0;
    int r = load_rows(c, db, "tessera_xty", arg[Y_T1], arg[Y_K1], arg[Y_X1], has_v ? arg[Y_V1] : 0, 0, &kkind, &xkind);
    if (!r) r = load_rows(c, db, "tessera_xty", arg[Y_T2], arg[Y_K2], arg[Y_Y2], has_v ? arg[Y_V2] : 0, 1, &kkind, &ykind);
    if (r == LOAD_REAL) continue;
    if (r) return r;
    break;
  }
  return run_query_c(c, db, 0, minval, threads);
}

static sqlite3_module tesseraCrossModule = {
    .iVersion = 0,
    .xCreate = 0,
    .xConnect = yConnect,
    .xBestIndex = yBestIndex,
    .xDisconnect = tDisconnect,
    .xOpen = tOpen,
    .xClose = tClose,
    .xFilter = yFilter,
    .xNext = tNext,
    .xEof = tEof,
    .xColumn = tColumn,
    .xRowid = tRowid,
};

enum { S_X0, S_X1, S_S, S_TBL, S_K, S_X, S_MEASURE, S_T, S_V, S_MODE, S_THREADS };

static int sConnect(sqlite3 *db, void *aux, int argc, const char *const *argv, sqlite3_vtab **pp, char **err) {
  int rc = sqlite3_declare_vtab(db,
      "CREATE TABLE x(x0, x1, s, tbl HIDDEN, kcol HIDDEN, xcol HIDDEN, measure HIDDEN, threshold HIDDEN, vcol HIDDEN, "
      "mode HIDDEN, threads HIDDEN)");
  if (rc != SQLITE_OK) return rc;
#ifdef SQLITE_VTAB_DIRECTONLY
  sqlite3_vtab_config(db, SQLITE_VTAB_DIRECTONLY);
#endif
  TVtab *v = sqlite3_malloc(sizeof *v);
  if (!v) return SQLITE_NOMEM;
  memset(v, 0, sizeof *v);
  v->db = db;
  *pp = &v->base;
  return SQLITE_OK;
}

static int sBestIndex(sqlite3_vtab *tab, sqlite3_index_info *info) {
  return best_index(tab, info, S_TBL, S_THREADS, 5,
                    "tessera_sim(table, k column, x column, measure, threshold [, v column, mode, threads])");
}

static int sFilter(sqlite3_vtab_cursor *cur, int mask, const char *idxStr, int argc, sqlite3_value **argv) {
  TCur *c = (TCur *)cur;
  sqlite3 *db = ((TVtab *)cur->pVtab)->db;
  const char *arg[S_THREADS + 1] = {0};
  sqlite3_value *val[S_THREADS + 1] = {0};
  for (int col = S_TBL, a = 0; col <= S_THREADS; col++)
    if (mask & (1 << (col - S_TBL))) {
      val[col] = argv[a++];
      arg[col] = (const char *)sqlite3_value_text(val[col]);
    }
  if (!tessera_c_supported()) return fail(c, "tessera_sim: this CPU lacks x86-64-v3 (AVX2, BMI2, FMA), which Tessera needs");
  if (!arg[S_TBL] || !arg[S_K] || !arg[S_X] || !val[S_T] || sqlite3_value_type(val[S_T]) == SQLITE_NULL)
    return fail(c, "tessera_sim: the table, k, x and threshold arguments must not be NULL");
  const char *ms = arg[S_MEASURE];
  const int measure = !ms ? 0 : !strcmp(ms, "cosine") ? 1 : !strcmp(ms, "jaccard") ? 2 : !strcmp(ms, "dice") ? 3
                              : !strcmp(ms, "overlap") ? 4 : 0;
  if (!measure) return fail(c, "tessera_sim: measure is 'cosine', 'jaccard', 'dice' or 'overlap'");
  int mode = 1;
  if (arg[S_MODE]) {
    if (strcmp(arg[S_MODE], "full") == 0) mode = 0;
    else if (strcmp(arg[S_MODE], "le") == 0) mode = 2;
    else if (strcmp(arg[S_MODE], "lt") != 0) return fail(c, "tessera_sim: mode is 'lt', 'le' or 'full'");
  }
  const int has_v = val[S_V] && sqlite3_value_type(val[S_V]) != SQLITE_NULL;
  const int threads = val[S_THREADS] && sqlite3_value_type(val[S_THREADS]) != SQLITE_NULL ? sqlite3_value_int(val[S_THREADS]) : 0;
  free_query(c);
  c->q = tessera_c_new(has_v);
  c->real = 0;
  c->int_v = 0;
  c->cross = 0;
  c->sim_measure = measure;
  c->any_class = 1;
  c->min_sim = sqlite3_value_double(val[S_T]);
  if (!c->q) return fail_rc(c, "tessera_sim: out of memory", SQLITE_NOMEM);
  int kkind = 0, xkind = 0;
  const int r = load_rows(c, db, "tessera_sim", arg[S_TBL], arg[S_K], arg[S_X], has_v ? arg[S_V] : 0, 0, &kkind, &xkind);
  if (r) return r;
  if (mode && xkind == 2 && !db_utf8(db))
    return fail(c, "tessera_sim: mode 'lt' or 'le' on TEXT items needs a UTF-8 database (BINARY orders UTF-16 text by "
                   "its UTF-16 bytes)");
  return run_query_c(c, db, mode, 0, threads);
}

static sqlite3_module tesseraSimModule = {
    .iVersion = 0,
    .xCreate = 0,
    .xConnect = sConnect,
    .xBestIndex = sBestIndex,
    .xDisconnect = tDisconnect,
    .xOpen = tOpen,
    .xClose = tClose,
    .xFilter = sFilter,
    .xNext = tNext,
    .xEof = tEof,
    .xColumn = tColumn,
    .xRowid = tRowid,
};

#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_tessera_init(sqlite3 *db, char **err, const sqlite3_api_routines *api) {
  SQLITE_EXTENSION_INIT2(api);
  if (tessera_c_api_version() != TESSERA_API_VERSION) {
    *err = sqlite3_mprintf("tessera: core interface version %d, this extension needs %d", tessera_c_api_version(),
                           TESSERA_API_VERSION);
    return SQLITE_ERROR;
  }
  int rc = sqlite3_create_module(db, "tessera_xtx", &tesseraModule, 0);
  if (rc == SQLITE_OK) rc = sqlite3_create_module(db, "tessera_xty", &tesseraCrossModule, 0);
  return rc == SQLITE_OK ? sqlite3_create_module(db, "tessera_sim", &tesseraSimModule, 0) : rc;
}
