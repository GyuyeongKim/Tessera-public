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
  int distinct;
  int nk;
  int int_v;
  int any_class;
  int cross;
  int sim_measure;
  double min_sim;
  int64_t x0, x1, vi, rowid;
  double vd;
  int xnull[2], xint[2], null_skip;
  int *kkind;
} TCur;

static void free_query(TCur *c) {
  if (c->q) tessera_c_free(c->q);
  if (c->dk) tessera_dict_free(c->dk);
  if (c->dx) tessera_dict_free(c->dx);
  if (c->dy) tessera_dict_free(c->dy);
  sqlite3_free(c->kkind);
  c->q = 0;
  c->dk = c->dx = c->dy = 0;
  c->kkind = 0;
}

static int host_is_interrupted;
static int interrupted(void *db) { return host_is_interrupted && sqlite3_is_interrupted((sqlite3 *)db); }

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

static void free_names(char **v, int n) {
  if (!v) return;
  for (int i = 0; i < n; i++) sqlite3_free(v[i]);
  sqlite3_free(v);
}

static int key_columns(sqlite3 *db, const char *tbl, const char *kc, char ***names) {
  const int one = !strchr(kc, ',') || (sqlite3_table_column_metadata != 0 &&
                                       sqlite3_table_column_metadata(db, 0, tbl, kc, 0, 0, 0, 0, 0) == SQLITE_OK);
  int n = 1;
  if (!one)
    for (const char *p = kc; *p; p++) n += *p == ',';
  char **v = sqlite3_malloc64(sizeof(char *) * (sqlite3_uint64)n);
  *names = v;
  if (!v) return -2;
  memset(v, 0, sizeof(char *) * (size_t)n);
  int r = n;
  if (one) {
    if (!(v[0] = sqlite3_mprintf("%s", kc))) r = -2;
  } else {
    const char *p = kc;
    for (int i = 0; i < n && r > 0; i++) {
      while (*p == ' ') p++;
      const char *e = strchr(p, ',');
      size_t len = e ? (size_t)(e - p) : strlen(p);
      while (len > 0 && p[len - 1] == ' ') len--;
      if (len == 0 || len > 0x7fffffff) r = -1;
      else if (!(v[i] = sqlite3_mprintf("%.*s", (int)len, p))) r = -2;
      p = e ? e + 1 : p + len;
    }
  }
  if (r < 0) {
    free_names(v, n);
    *names = 0;
  }
  return r;
}

static int tuple_key(sqlite3_stmt *st, int nk, int *kind, tessera_dict_c **d, int64_t *out, const int *text_ok,
                     char **buf, size_t *cap) {
  size_t len = 0;
  for (int i = 0; i < nk; i++) {
    const int t = sqlite3_column_type(st, i);
    const int k = t == SQLITE_INTEGER ? 1 : t == SQLITE_TEXT ? 2 : 0;
    if (!k || (kind[i] && kind[i] != k)) return 1;
    if (k == 2 && !text_ok[i]) return 5;
    kind[i] = k;
    const size_t need = k == 1 ? 8 : 4 + (size_t)sqlite3_column_bytes(st, i);
    if (len + need > *cap) {
      size_t c = *cap ? *cap : 256;
      while (c < len + need) c *= 2;
      char *b = realloc(*buf, c);
      if (!b) return 2;
      *buf = b;
      *cap = c;
    }
    if (k == 1) {
      const int64_t v = sqlite3_column_int64(st, i);
      memcpy(*buf + len, &v, 8);
      len += 8;
    } else {
      const unsigned char *s = sqlite3_column_text(st, i);
      const uint32_t n = (uint32_t)sqlite3_column_bytes(st, i);
      memcpy(*buf + len, &n, 4);
      memcpy(*buf + len + 4, s, n);
      len += 4 + n;
    }
  }
  if (!*d && !(*d = tessera_dict_new())) return 2;
  *out = tessera_dict_code(*d, *buf, len);
  return *out < 0 ? 2 : 0;
}

static int may_hold_null(sqlite3 *db, const char *tbl, const char *col) {
  int notnull = 0;
  if (sqlite3_table_column_metadata != 0 &&
      sqlite3_table_column_metadata(db, 0, tbl, col, 0, 0, &notnull, 0, 0) == SQLITE_OK && notnull)
    return 0;
  char *sql = sqlite3_mprintf("SELECT 1 FROM \"%w\" WHERE \"%w\" IS NULL LIMIT 1", tbl, col);
  if (!sql) return 1;
  sqlite3_stmt *st = 0;
  int found = 1;
  if (sqlite3_prepare_v2(db, sql, -1, &st, 0) == SQLITE_OK) found = sqlite3_step(st) != SQLITE_DONE;
  sqlite3_finalize(st);
  sqlite3_free(sql);
  return found;
}

static int null_group_code(sqlite3_stmt *st, int i, int *kind, tessera_dict_c **d, int64_t *out, int text_ok,
                           char **buf, size_t *cap) {
  const int t = sqlite3_column_type(st, i);
  if (!*d && !(*d = tessera_dict_new())) return 2;
  if (t == SQLITE_NULL) {
    *out = tessera_dict_code(*d, "", 1);
    return *out < 0 ? 2 : 0;
  }
  const int k = t == SQLITE_INTEGER ? 1 : t == SQLITE_TEXT ? 2 : 0;
  if (!k || (*kind && *kind != k)) return 1;
  if (k == 2 && !text_ok) return 5;
  *kind = k;
  const size_t len = k == 1 ? sizeof(int64_t) : (size_t)sqlite3_column_bytes(st, i);
  if (len + 1 > *cap) {
    char *nb = realloc(*buf, len + 1);
    if (!nb) return 2;
    *buf = nb;
    *cap = len + 1;
  }
  (*buf)[0] = 1;
  if (k == 1) {
    const int64_t v = sqlite3_column_int64(st, i);
    memcpy(*buf + 1, &v, sizeof v);
  } else {
    memcpy(*buf + 1, sqlite3_column_text(st, i), len);
  }
  *out = tessera_dict_code(*d, *buf, len + 1);
  return *out < 0 ? 2 : 0;
}

static int load_rows(TCur *c, sqlite3 *db, const char *fn, const char *tbl, const char *kc, const char *xc, const char *vc,
                     int side, int *xkind) {
  char **kn = 0;
  const int nk = key_columns(db, tbl, kc, &kn);
  char msg[300];
  if (nk < 0) {
    if (nk == -2) return fail_rc(c, "tessera: out of memory", SQLITE_NOMEM);
    snprintf(msg, sizeof msg, "%s: the key columns are names separated by commas", fn);
    return fail(c, msg);
  }
  if (side == 0) {
    c->nk = nk;
    sqlite3_free(c->kkind);
    if (!(c->kkind = sqlite3_malloc64(sizeof(int) * (sqlite3_uint64)nk))) {
      free_names(kn, nk);
      return fail_rc(c, "tessera: out of memory", SQLITE_NOMEM);
    }
    memset(c->kkind, 0, sizeof(int) * (size_t)nk);
  } else if (nk != c->nk) {
    free_names(kn, nk);
    snprintf(msg, sizeof msg, "%s: the two key column lists have different lengths", fn);
    return fail(c, msg);
  }
  int *kkind = c->kkind;
  const char **cols = sqlite3_malloc64(sizeof(char *) * (sqlite3_uint64)(nk + 1));
  int *text_ok = sqlite3_malloc64(sizeof(int) * (sqlite3_uint64)(nk + 1));
  char *sql = 0;
  int rc = SQLITE_OK;
  if (cols && text_ok) {
    for (int i = 0; i < nk; i++) cols[i] = kn[i];
    cols[nk] = xc;
    for (int i = 0; i <= nk && rc == SQLITE_OK; i++) {
      const char *coll = 0;
      text_ok[i] = sqlite3_table_column_metadata != 0 &&
                   sqlite3_table_column_metadata(db, 0, tbl, cols[i], 0, &coll, 0, 0, 0) == SQLITE_OK;
      if (text_ok[i] && coll && sqlite3_stricmp(coll, "BINARY") != 0) {
        snprintf(msg, sizeof msg, "%s: column %s has collation %s; only BINARY is supported", fn, cols[i], coll);
        rc = SQLITE_ERROR;
      }
    }
    sql = rc == SQLITE_OK ? sqlite3_mprintf("SELECT ") : 0;
    for (int i = 0; sql && rc == SQLITE_OK && i <= nk + (vc ? 1 : 0); i++) {
      char *n2 = sqlite3_mprintf("%s%s\"%w\"", sql, i ? ", " : "", i <= nk ? cols[i] : vc);
      sqlite3_free(sql);
      sql = n2;
    }
    if (sql && rc == SQLITE_OK) {
      char *n2 = sqlite3_mprintf("%s FROM \"%w\"", sql, tbl);
      sqlite3_free(sql);
      sql = n2;
    }
  }
  if (rc != SQLITE_OK || !sql) {
    sqlite3_free(sql);
    sqlite3_free(cols);
    sqlite3_free(text_ok);
    free_names(kn, nk);
    return rc != SQLITE_OK ? fail(c, msg) : fail_rc(c, "tessera: out of memory", SQLITE_NOMEM);
  }
  sqlite3_stmt *st = 0;
  rc = sqlite3_prepare_v2(db, sql, -1, &st, 0);
  sqlite3_free(sql);
  if (rc != SQLITE_OK) {
    sqlite3_free(cols);
    sqlite3_free(text_ok);
    free_names(kn, nk);
    return fail_rc(c, sqlite3_errmsg(db), rc);
  }
  const int xi = nk, vi = nk + 1;
  char *tbuf = 0;
  size_t tcap = 0;
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
    int null_key = 0;
    for (int i = 0; i < nk; i++) null_key |= sqlite3_column_type(st, i) == SQLITE_NULL;
    if (null_key) continue;
    if (sqlite3_column_type(st, xi) == SQLITE_NULL && c->null_skip) continue;
    if (sqlite3_column_type(st, xi) == SQLITE_NULL && !c->xnull[side]) { bad = 1; break; }
    if (vc && sqlite3_column_type(st, vi) == SQLITE_NULL) { bad = 9; break; }
    const int e = nk == 1 ? key_of(st, 0, &kkind[0], &c->dk, &kb[n], text_ok[0])
                          : tuple_key(st, nk, kkind, &c->dk, &kb[n], text_ok, &tbuf, &tcap);
    const int f = e ? e
                : c->xnull[side] ? null_group_code(st, xi, xkind, dxs, &xb[n], text_ok[nk], &tbuf, &tcap)
                                 : key_of(st, xi, xkind, dxs, &xb[n], text_ok[nk]);
    if (e || f) { const int g = e ? e : f; bad = g == 2 ? 2 : g == 5 ? (e ? 5 : 6) : 3; break; }
    const int vt = vc ? sqlite3_column_type(st, vi) : 0;
    if (vc && vt != SQLITE_INTEGER && vt != SQLITE_FLOAT) { bad = 7; break; }
    if (iv) {
      if (vt != SQLITE_INTEGER) { bad = 4; break; }
      vib[n] = sqlite3_column_int64(st, vi);
    } else if (vc) {
      if (vt == SQLITE_INTEGER && !c->any_class) { bad = 8; break; }
      vb[n] = sqlite3_column_double(st, vi);
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
  free(tbuf);
  sqlite3_free(cols);
  sqlite3_free(text_ok);
  free_names(kn, nk);
  if (bad == 4) return LOAD_REAL;
  if (bad == 1) { snprintf(msg, sizeof msg, "%s: NULL group keys are not supported here", fn); return fail(c, msg); }
  if (bad == 9) { snprintf(msg, sizeof msg, "%s: NULL values are not supported", fn); return fail(c, msg); }
  c->xint[side] = *xkind != 2;
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
  tessera_c_set_distinct_keys(c->q, c->distinct);
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
  int mode = 0, distinct = 0;
  if (arg[C_MODE]) {
    const char *m = arg[C_MODE], *sp = strchr(m, ' ');
    const size_t n = sp ? (size_t)(sp - m) : strlen(m);
    if (n == 2 && strncmp(m, "lt", 2) == 0) mode = 1;
    else if (n == 2 && strncmp(m, "le", 2) == 0) mode = 2;
    else if (n == 2 && strncmp(m, "ne", 2) == 0) mode = 3;
    else if (!(n == 4 && strncmp(m, "full", 4) == 0)) mode = -1;
    if (sp && strcmp(sp, " distinct") == 0) distinct = 1;
    else if (sp) mode = -1;
    if (mode < 0) return fail(c, "tessera_xtx: mode is 'full', 'lt', 'le' or 'ne', optionally followed by ' distinct'");
  }
  if (distinct && has_v) return fail(c, "tessera_xtx: mode '... distinct' counts keys, without a v column");
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
    c->distinct = distinct;
    c->null_skip = mode != 0;
    c->xnull[0] = mode == 0 && may_hold_null(db, arg[C_TBL], arg[C_X]);
    c->xnull[1] = 0;
    if (!c->q) return fail_rc(c, "tessera_xtx: out of memory", SQLITE_NOMEM);
    int xkind = 0;
    const int r = load_rows(c, db, "tessera_xtx", arg[C_TBL], arg[C_K], arg[C_X], has_v ? arg[C_V] : 0, 0, &xkind);
    if (r == LOAD_REAL) continue;
    if (r) return r;
    if ((mode == 1 || mode == 2) && xkind == 2 && !db_utf8(db))
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
      const int side = c->cross && col == C_X1;
      size_t len;
      const char *s = side ? (c->dy ? tessera_c_ystr(c->q, x, &len) : 0) : (c->dx ? tessera_c_xstr(c->q, x, &len) : 0);
      if (s && c->xnull[side]) {
        int64_t v;
        if (s[0] == 0) sqlite3_result_null(ctx);
        else if (c->xint[side] && len == 1 + sizeof v) {
          memcpy(&v, s + 1, sizeof v);
          sqlite3_result_int64(ctx, v);
        } else sqlite3_result_text(ctx, s + 1, (int)len - 1, SQLITE_TRANSIENT);
      } else if (s) sqlite3_result_text(ctx, s, (int)len, SQLITE_TRANSIENT);
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
    c->null_skip = 0;
    c->xnull[0] = may_hold_null(db, arg[Y_T1], arg[Y_X1]);
    c->xnull[1] = may_hold_null(db, arg[Y_T2], arg[Y_Y2]);
    if (!c->q) return fail_rc(c, "tessera_xty: out of memory", SQLITE_NOMEM);
    int xkind = 0, ykind = 0;
    int r = load_rows(c, db, "tessera_xty", arg[Y_T1], arg[Y_K1], arg[Y_X1], has_v ? arg[Y_V1] : 0, 0, &xkind);
    if (!r) r = load_rows(c, db, "tessera_xty", arg[Y_T2], arg[Y_K2], arg[Y_Y2], has_v ? arg[Y_V2] : 0, 1, &ykind);
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
    else if (strcmp(arg[S_MODE], "ne") == 0) mode = 3;
    else if (strcmp(arg[S_MODE], "lt") != 0) return fail(c, "tessera_sim: mode is 'lt', 'le', 'full' or 'ne'");
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
  c->null_skip = 1;
  c->xnull[0] = c->xnull[1] = 0;
  int xkind = 0;
  const int r = load_rows(c, db, "tessera_sim", arg[S_TBL], arg[S_K], arg[S_X], has_v ? arg[S_V] : 0, 0, &xkind);
  if (r) return r;
  if ((mode == 1 || mode == 2) && xkind == 2 && !db_utf8(db))
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
  host_is_interrupted = sqlite3_libversion_number() >= 3041000;
  if (tessera_c_api_version() != TESSERA_API_VERSION) {
    *err = sqlite3_mprintf("tessera: core interface version %d, this extension needs %d", tessera_c_api_version(),
                           TESSERA_API_VERSION);
    return SQLITE_ERROR;
  }
  int rc = sqlite3_create_module(db, "tessera_xtx", &tesseraModule, 0);
  if (rc == SQLITE_OK) rc = sqlite3_create_module(db, "tessera_xty", &tesseraCrossModule, 0);
  return rc == SQLITE_OK ? sqlite3_create_module(db, "tessera_sim", &tesseraSimModule, 0) : rc;
}
