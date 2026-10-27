#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tessera_c.h"

static int fails = 0;
#define CHECK(c, ...)                                   \
  do {                                                  \
    if (!(c)) {                                         \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
      fprintf(stderr, __VA_ARGS__);                     \
      fprintf(stderr, "\n");                            \
      fails++;                                          \
      return;                                           \
    }                                                   \
  } while (0)

static uint64_t rng = 88172645463325252ull;
static uint64_t next_u64(void) {
  rng ^= rng << 13;
  rng ^= rng >> 7;
  rng ^= rng << 17;
  return rng;
}

typedef struct {
  int64_t x0, x1;
  double v;
} Row;

static int value_kind = 0, wide_runs = 0;
static double draw_value(int m) {
  if (value_kind == 1) return 256 + (double)(next_u64() % 3840);
  if (value_kind == 2) return 65536 + (double)(next_u64() % 1000);
  return 1 + (double)(next_u64() % m);
}

static int cmp_row(const void *a, const void *b) {
  const Row *p = a, *q = b;
  if (p->x0 != q->x0) return p->x0 < q->x0 ? -1 : 1;
  if (p->x1 != q->x1) return p->x1 < q->x1 ? -1 : 1;
  return 0;
}

static int expect_general(const int64_t *k, const int64_t *x, const double *v, size_t n) {
  if (!v) return 0;
  for (size_t a = 0; a < n; a++) {
    double s = 0;
    for (size_t b = 0; b < n; b++)
      if (k[b] == k[a] && x[b] == x[a]) s += v[b];
    if (s < 1 || s > 65535 || s != (double)(int64_t)s) return 1;
  }
  return 0;
}

static size_t brute(const int64_t *k, const int64_t *x, const double *v, size_t n, int mode, double minv, Row **out) {
  size_t cap = n * n + 1, m = 0;
  Row *r = malloc(sizeof(Row) * cap);
  for (size_t a = 0; a < n; a++)
    for (size_t b = 0; b < n; b++) {
      if (k[a] != k[b]) continue;
      if (mode == 1 && !(x[a] < x[b])) continue;
      if (mode == 2 && !(x[a] <= x[b])) continue;
      r[m].x0 = x[a];
      r[m].x1 = x[b];
      r[m].v = v ? v[a] * v[b] : 1;
      m++;
    }
  qsort(r, m, sizeof(Row), cmp_row);
  size_t w = 0;
  for (size_t i = 0; i < m; i++) {
    if (w && r[w - 1].x0 == r[i].x0 && r[w - 1].x1 == r[i].x1) r[w - 1].v += r[i].v;
    else r[w++] = r[i];
  }
  size_t f = 0;
  for (size_t i = 0; i < w; i++)
    if (r[i].v >= minv) r[f++] = r[i];
  *out = r;
  return f;
}

static size_t collect(tessera_query_c *q, int batch, double minv, Row **out) {
  size_t cap = 1024, m = 0;
  Row *r = malloc(sizeof(Row) * cap);
  int64_t x0[7], x1[7], vi[7];
  double vd[7];
  for (;;) {
    size_t got;
    if (batch) {
      got = tessera_c_next_batch(q, x0, x1, vi, vd, 7);
    } else {
      int gen;
      got = tessera_c_next(q, &x0[0], &x1[0], &vi[0], &vd[0], &gen);
      if (got && !gen) vd[0] = (double)vi[0];
    }
    if (!got) break;
    for (size_t i = 0; i < got; i++) {
      if (vd[i] < minv) continue;
      if (m == cap) r = realloc(r, sizeof(Row) * (cap *= 2));
      r[m].x0 = x0[i];
      r[m].x1 = x1[i];
      r[m].v = vd[i];
      m++;
    }
  }
  qsort(r, m, sizeof(Row), cmp_row);
  *out = r;
  return m;
}

static int same(const Row *a, size_t na, const Row *b, size_t nb) {
  if (na != nb) return 0;
  for (size_t i = 0; i < na; i++)
    if (a[i].x0 != b[i].x0 || a[i].x1 != b[i].x1 || a[i].v != b[i].v) return 0;
  return 1;
}

static void test_wide(int mode, int with_v, int batch, size_t budget, double minv) {
  enum { N = 600 };
  int64_t kv[40], xv[30], k[N], x[N];
  double v[N];
  for (int i = 0; i < 40; i++) kv[i] = (int64_t)next_u64();
  for (int i = 0; i < 30; i++) xv[i] = (int64_t)next_u64();
  xv[0] = INT64_MIN;
  xv[1] = INT64_MAX;
  kv[0] = INT64_MAX;
  for (int i = 0; i < N; i++) {
    k[i] = kv[next_u64() % 40];
    x[i] = xv[(next_u64() % 30) * (next_u64() % 30) / 29];
    v[i] = draw_value(5);
  }
  tessera_query_c *q = tessera_c_new(with_v);
  CHECK(q && tessera_c_add(q, k, x, with_v ? v : NULL, N) == TESSERA_C_OK, "add");
  tessera_c_set_memory(q, budget, NULL);
  char err[256];
  int rc = tessera_c_run(q, mode, 1, (uint64_t)minv, -1, 4, -1, 0, err, sizeof err);
  CHECK(rc == TESSERA_C_OK, "run: %s", err);
  const int want_general = expect_general(k, x, with_v ? v : NULL, N);
  CHECK(tessera_c_general(q) == want_general, "wide keys value kind %d v %d: general %d, expected %d", value_kind, with_v,
        tessera_c_general(q), want_general);
  if (with_v && value_kind == 1 && !want_general) wide_runs++;
  Row *a, *b;
  size_t na = collect(q, batch, minv, &a), nb = brute(k, x, with_v ? v : NULL, N, mode, minv, &b);
  CHECK(same(a, na, b, nb), "wide keys mode %d v %d batch %d budget %zu value kind %d: %zu rows vs brute force %zu", mode,
        with_v, batch, budget, value_kind, na, nb);
  free(a);
  free(b);
  tessera_c_free(q);
}

static int str_rank(char pool[][8], int ns, const char *s) {
  int below = 0;
  for (int j = 0; j < ns; j++) {
    int dup = 0;
    for (int m = 0; m < j; m++) dup |= strcmp(pool[m], pool[j]) == 0;
    if (!dup) below += strcmp(pool[j], s) < 0;
  }
  return below;
}

static void test_strings(int mode, int with_v) {
  enum { N = 500, NS = 25 };
  char pool[NS][8];
  for (int i = 0; i < NS; i++) {
    int len = 1 + (int)(next_u64() % 6);
    for (int c = 0; c < len; c++) pool[i][c] = "ab\xc3\xa9z"[next_u64() % 5];
    pool[i][len] = 0;
  }
  tessera_dict_c *dk = tessera_dict_new(), *dx = tessera_dict_new();
  int64_t k[N], x[N], bk[N], bx[N];
  double v[N];
  for (int i = 0; i < N; i++) {
    const char *ks = pool[next_u64() % NS], *xs = pool[(next_u64() % NS) * (next_u64() % NS) / (NS - 1)];
    v[i] = 1 + (double)(next_u64() % 3);
    k[i] = tessera_dict_code(dk, ks, strlen(ks));
    x[i] = tessera_dict_code(dx, xs, strlen(xs));
    bk[i] = str_rank(pool, NS, ks);
    bx[i] = str_rank(pool, NS, xs);
  }
  tessera_query_c *q = tessera_c_new(with_v);
  CHECK(q && tessera_c_add(q, k, x, with_v ? v : NULL, N) == TESSERA_C_OK, "add");
  tessera_c_set_xdict(q, dx);
  char err[256];
  CHECK(tessera_c_run(q, mode, 1, 0, -1, 4, -1, 0, err, sizeof err) == TESSERA_C_OK, "run: %s", err);
  Row *a, *b;
  size_t na = collect(q, 0, 0, &a), nb = brute(bk, bx, with_v ? v : NULL, N, mode, 0, &b);
  for (size_t i = 0; i < na; i++) {
    size_t l0, l1;
    const char *s0 = tessera_c_xstr(q, a[i].x0, &l0), *s1 = tessera_c_xstr(q, a[i].x1, &l1);
    CHECK(s0 && s1 && l0 < 8 && l1 < 8, "xstr");
    char t0[8], t1[8];
    memcpy(t0, s0, l0);
    t0[l0] = 0;
    memcpy(t1, s1, l1);
    t1[l1] = 0;
    a[i].x0 = str_rank(pool, NS, t0);
    a[i].x1 = str_rank(pool, NS, t1);
  }
  qsort(a, na, sizeof(Row), cmp_row);
  CHECK(same(a, na, b, nb), "string keys mode %d v %d: %zu rows vs brute force %zu", mode, with_v, na, nb);
  free(a);
  free(b);
  tessera_c_free(q);
  tessera_dict_free(dk);
  tessera_dict_free(dx);
}

typedef struct {
  int64_t k, x;
  double v;
} KRow;
static int cmp_krow(const void *a, const void *b) {
  const KRow *p = a, *q = b;
  return p->k < q->k ? -1 : p->k > q->k;
}
static size_t brute_cross(const int64_t *rk, const int64_t *rx, const double *rv, size_t nr, const int64_t *sk,
                          const int64_t *sy, const double *sw, size_t ns, double minv, Row **out) {
  KRow *a = malloc(sizeof(KRow) * nr), *b = malloc(sizeof(KRow) * ns);
  for (size_t i = 0; i < nr; i++) a[i] = (KRow){rk[i], rx[i], rv ? rv[i] : 1};
  for (size_t i = 0; i < ns; i++) b[i] = (KRow){sk[i], sy[i], sw ? sw[i] : 1};
  qsort(a, nr, sizeof(KRow), cmp_krow);
  qsort(b, ns, sizeof(KRow), cmp_krow);
  size_t cap = 1024, m = 0;
  Row *r = malloc(sizeof(Row) * cap);
  for (size_t i = 0, j = 0; i < nr && j < ns;) {
    if (a[i].k < b[j].k) { i++; continue; }
    if (a[i].k > b[j].k) { j++; continue; }
    size_t i1 = i, j1 = j;
    while (i1 < nr && a[i1].k == a[i].k) i1++;
    while (j1 < ns && b[j1].k == b[j].k) j1++;
    for (size_t p = i; p < i1; p++)
      for (size_t q = j; q < j1; q++) {
        if (m == cap) r = realloc(r, sizeof(Row) * (cap *= 2));
        r[m++] = (Row){a[p].x, b[q].x, a[p].v * b[q].v};
      }
    i = i1;
    j = j1;
  }
  qsort(r, m, sizeof(Row), cmp_row);
  size_t w = 0;
  for (size_t i = 0; i < m; i++) {
    if (w && r[w - 1].x0 == r[i].x0 && r[w - 1].x1 == r[i].x1) r[w - 1].v += r[i].v;
    else r[w++] = r[i];
  }
  size_t f = 0;
  for (size_t i = 0; i < w; i++)
    if (r[i].v >= minv) r[f++] = r[i];
  free(a);
  free(b);
  *out = r;
  return f;
}

static void test_cross(int with_v, int batch, size_t budget, double minv, int64_t frontier, int nr, int ns, int nk,
                       int nx, int ny) {
  int64_t *rk = malloc(8 * nr), *rx = malloc(8 * nr), *sk = malloc(8 * ns), *sy = malloc(8 * ns);
  double *rv = malloc(8 * nr), *sw = malloc(8 * ns);
  const int64_t kmul = 1000000007, xoff = INT64_MIN + 12345, ymul = -999999937;
  for (int i = 0; i < nr; i++) {
    rk[i] = (int64_t)(next_u64() % nk) * kmul;
    rx[i] = xoff + (int64_t)((next_u64() % nx) * (next_u64() % nx) / nx);
    rv[i] = draw_value(4);
  }
  for (int i = 0; i < ns; i++) {
    sk[i] = (int64_t)(next_u64() % nk) * kmul;
    sy[i] = (int64_t)((next_u64() % ny) * (next_u64() % ny) / ny) * ymul;
    sw[i] = draw_value(3);
  }
  tessera_query_c *q = tessera_c_new_cross(with_v);
  CHECK(q && tessera_c_add(q, rk, rx, with_v ? rv : NULL, nr) == TESSERA_C_OK &&
            tessera_c_add_y(q, sk, sy, with_v ? sw : NULL, ns) == TESSERA_C_OK, "cross add");
  tessera_c_set_memory(q, budget, NULL);
  char err[256];
  CHECK(tessera_c_run(q, 0, 1, (uint64_t)minv, -1, 4, frontier, 0, err, sizeof err) == TESSERA_C_OK, "cross run: %s", err);
  const int want_general = with_v && (expect_general(rk, rx, rv, nr) || expect_general(sk, sy, sw, ns));
  CHECK(tessera_c_general(q) == want_general, "cross value kind %d v %d: general %d, expected %d", value_kind, with_v,
        tessera_c_general(q), want_general);
  if (with_v && value_kind == 1 && !want_general) wide_runs++;
  Row *a, *b;
  size_t na = collect(q, batch, minv, &a);
  size_t nb = brute_cross(rk, rx, with_v ? rv : NULL, nr, sk, sy, with_v ? sw : NULL, ns, minv, &b);
  CHECK(same(a, na, b, nb), "cross v %d batch %d budget %zu having %g frontier %lld value kind %d: %zu rows vs brute force %zu",
        with_v, batch, budget, minv, (long long)frontier, value_kind, na, nb);
  free(a);
  free(b);
  tessera_c_free(q);
  free(rk); free(rx); free(sk); free(sy); free(rv); free(sw);
}

static void test_cross_strings(int with_v) {
  enum { N = 3000, NS = 40 };
  char pool[NS][8];
  for (int i = 0; i < NS; i++) {
    int len = 1 + (int)(next_u64() % 6);
    for (int c = 0; c < len; c++) pool[i][c] = "ab\xc3\xa9z"[next_u64() % 5];
    pool[i][len] = 0;
  }
  tessera_dict_c *dk = tessera_dict_new(), *dx = tessera_dict_new(), *dy = tessera_dict_new();
  int64_t rk[N], rx[N], sk[N], sy[N], brk[N], brx[N], bsk[N], bsy[N];
  double rv[N], sw[N];
  for (int i = 0; i < N; i++) {
    const char *a = pool[next_u64() % NS], *b = pool[next_u64() % NS], *c = pool[next_u64() % NS], *d = pool[next_u64() % NS];
    rk[i] = tessera_dict_code(dk, a, strlen(a));
    rx[i] = tessera_dict_code(dx, b, strlen(b));
    sk[i] = tessera_dict_code(dk, c, strlen(c));
    sy[i] = tessera_dict_code(dy, d, strlen(d));
    brk[i] = str_rank(pool, NS, a);
    brx[i] = str_rank(pool, NS, b);
    bsk[i] = str_rank(pool, NS, c);
    bsy[i] = str_rank(pool, NS, d);
    rv[i] = 1 + (double)(next_u64() % 3);
    sw[i] = 0.5 * (double)(next_u64() % 5);
  }
  tessera_query_c *q = tessera_c_new_cross(with_v);
  CHECK(q && tessera_c_add(q, rk, rx, with_v ? rv : NULL, N) == 0 && tessera_c_add_y(q, sk, sy, with_v ? sw : NULL, N) == 0,
        "add");
  tessera_c_set_xdict(q, dx);
  tessera_c_set_ydict(q, dy);
  char err[256];
  CHECK(tessera_c_run(q, 0, 1, 0, -1, 4, -1, 0, err, sizeof err) == TESSERA_C_OK, "run: %s", err);
  Row *a, *b;
  size_t na = collect(q, 1, -1e300, &a), nb = brute_cross(brk, brx, with_v ? rv : NULL, N, bsk, bsy, with_v ? sw : NULL, N, -1e300, &b);
  for (size_t i = 0; i < na; i++) {
    size_t l0, l1;
    const char *s0 = tessera_c_xstr(q, a[i].x0, &l0), *s1 = tessera_c_ystr(q, a[i].x1, &l1);
    CHECK(s0 && s1 && l0 < 8 && l1 < 8, "xstr / ystr");
    char t0[8], t1[8];
    memcpy(t0, s0, l0);
    t0[l0] = 0;
    memcpy(t1, s1, l1);
    t1[l1] = 0;
    a[i].x0 = str_rank(pool, NS, t0);
    a[i].x1 = str_rank(pool, NS, t1);
  }
  qsort(a, na, sizeof(Row), cmp_row);
  CHECK(same(a, na, b, nb), "cross strings v %d: %zu rows vs brute force %zu", with_v, na, nb);
  free(a);
  free(b);
  tessera_c_free(q);
  tessera_dict_free(dk);
  tessera_dict_free(dx);
  tessera_dict_free(dy);
}

static double sim_of(int m, double c, double ni, double nj) {
  switch (m) {
    case 1: return c / __builtin_sqrt(ni * nj);
    case 2: return c / (ni + nj - c);
    case 3: return 2 * c / (ni + nj);
    default: return c / (ni < nj ? ni : nj);
  }
}

static void test_similarity(int measure, double t, int mode, int with_v, int dups, int64_t frontier) {
  enum { N = 30000, NK = 600, NX = 400 };
  static int64_t k[N], x[N];
  static double v[N];
  for (int i = 0; i < N; i++) {
    k[i] = (int64_t)(next_u64() % NK);
    x[i] = (int64_t)((next_u64() % NX) * (next_u64() % NX) / NX);
    v[i] = with_v ? 1 + (double)(next_u64() % 3) : 1;
  }
  int n = N;
  if (!dups) {
    KRow *p = malloc(sizeof(KRow) * N);
    for (int i = 0; i < N; i++) p[i] = (KRow){k[i] * 1000003 + x[i], i, 0};
    qsort(p, N, sizeof(KRow), cmp_krow);
    n = 0;
    for (int i = 0; i < N; i++)
      if (i == 0 || p[i].k != p[i - 1].k) { k[n] = k[p[i].x]; x[n] = x[p[i].x]; v[n] = 1; n++; }
    free(p);
  }
  Row *b;
  size_t nb = brute(k, x, with_v ? v : NULL, n, 0, -1e300, &b);
  size_t nx = 0;
  int64_t *ix = malloc(sizeof(int64_t) * nb);
  double *sz = malloc(sizeof(double) * nb);
  for (size_t i = 0; i < nb; i++)
    if (b[i].x0 == b[i].x1) { ix[nx] = b[i].x0; sz[nx++] = b[i].v; }
  size_t m = 0;
  for (size_t i = 0; i < nb; i++) {
    if (mode == 1 && b[i].x0 >= b[i].x1) continue;
    if (mode == 2 && b[i].x0 > b[i].x1) continue;
    double ni = 0, nj = 0;
    for (size_t q = 0; q < nx; q++) {
      if (ix[q] == b[i].x0) ni = sz[q];
      if (ix[q] == b[i].x1) nj = sz[q];
    }
    const double s = sim_of(measure, b[i].v, ni, nj);
    if (s >= t) b[m++] = (Row){b[i].x0, b[i].x1, s};
  }
  tessera_query_c *q = tessera_c_new(with_v);
  CHECK(q && tessera_c_add(q, k, x, with_v ? v : NULL, n) == 0, "add");
  tessera_c_set_similarity(q, measure, t);
  char err[256];
  CHECK(tessera_c_run(q, mode, 1, 0, -1, 4, frontier, 0, err, sizeof err) == TESSERA_C_OK, "run: %s", err);
  CHECK(tessera_c_general(q) == 1, "similarity values are doubles");
  Row *a;
  size_t na = collect(q, 1, -1e300, &a);
  CHECK(same(a, na, b, m), "similarity m %d t %g mode %d v %d dups %d frontier %lld: %zu rows vs brute force %zu", measure,
        t, mode, with_v, dups, (long long)frontier, na, m);
  free(a);
  free(b);
  free(ix);
  free(sz);
  tessera_c_free(q);
}

static void test_similarity_nested(int measure, double t, int mode, int64_t frontier) {
  enum { NK = 2000, NP = 30, NC = 19, CAP = NP * (NC + 1) * 200 };
  int64_t *k = malloc(sizeof(int64_t) * CAP), *x = malloc(sizeof(int64_t) * CAP);
  int n = 0;
  int *rows = malloc(sizeof(int) * NK);
  for (int p = 0; p < NP; p++) {
    const int sp = 20 + (int)(next_u64() % 180);
    for (int i = 0; i < NK; i++) rows[i] = i;
    for (int i = 0; i < sp; i++) {
      const int j = i + (int)(next_u64() % (NK - i));
      const int tmp = rows[i]; rows[i] = rows[j]; rows[j] = tmp;
      k[n] = rows[i]; x[n] = p * 100; n++;
    }
    for (int c = 1; c <= NC; c++) {
      const int sc = (sp * c + 19) / 20;
      for (int i = 0; i < sc; i++) {
        const int j = i + (int)(next_u64() % (sp - i));
        const int tmp = rows[i]; rows[i] = rows[j]; rows[j] = tmp;
        k[n] = rows[i]; x[n] = p * 100 + c; n++;
      }
    }
  }
  free(rows);
  Row *b;
  size_t nb = brute_cross(k, x, NULL, n, k, x, NULL, n, -1e300, &b);
  size_t nx = 0;
  int64_t *ix = malloc(sizeof(int64_t) * nb);
  double *sz = malloc(sizeof(double) * nb);
  for (size_t i = 0; i < nb; i++)
    if (b[i].x0 == b[i].x1) { ix[nx] = b[i].x0; sz[nx++] = b[i].v; }
  size_t m = 0;
  for (size_t i = 0; i < nb; i++) {
    if (mode == 1 && b[i].x0 >= b[i].x1) continue;
    if (mode == 2 && b[i].x0 > b[i].x1) continue;
    double ni = 0, nj = 0;
    for (size_t q = 0; q < nx; q++) {
      if (ix[q] == b[i].x0) ni = sz[q];
      if (ix[q] == b[i].x1) nj = sz[q];
    }
    const double s = sim_of(measure, b[i].v, ni, nj);
    if (s >= t) b[m++] = (Row){b[i].x0, b[i].x1, s};
  }
  tessera_query_c *q = tessera_c_new(0);
  CHECK(q && tessera_c_add(q, k, x, NULL, n) == 0, "add");
  tessera_c_set_similarity(q, measure, t);
  char err[256];
  CHECK(tessera_c_run(q, mode, 1, 0, -1, 4, frontier, 0, err, sizeof err) == TESSERA_C_OK, "run: %s", err);
  Row *a;
  size_t na = collect(q, 1, -1e300, &a);
  CHECK(same(a, na, b, m), "nested similarity m %d t %g mode %d frontier %lld: %zu rows vs brute force %zu", measure, t,
        mode, (long long)frontier, na, m);
  free(a); free(b); free(ix); free(sz); free(k); free(x);
  tessera_c_free(q);
}

static void test_bag(int mode, double minv) {
  enum { N = 2000 };
  int64_t k[N], x[N];
  for (int i = 0; i < N; i++) {
    if (i < 1500) {
      k[i] = (int64_t)(next_u64() % 2);
      x[i] = (int64_t)(next_u64() % 2);
    } else {
      k[i] = (int64_t)(next_u64() % 50);
      x[i] = 2 + (int64_t)(next_u64() % 40);
    }
  }
  tessera_query_c *q = tessera_c_new(0);
  CHECK(q && tessera_c_add(q, k, x, NULL, N) == TESSERA_C_OK, "bag add");
  char err[256];
  CHECK(tessera_c_run(q, mode, 1, (uint64_t)(minv > 0 ? minv : 0), -1, 4, -1, 0, err, sizeof err) == TESSERA_C_OK,
        "bag run: %s", err);
  CHECK(tessera_c_general(q) == 0, "bag counts are integers");
  Row *a, *b;
  size_t na = collect(q, 1, minv, &a), nb = brute(k, x, NULL, N, mode, minv, &b);
  CHECK(same(a, na, b, nb), "bag mode %d having %g: %zu rows vs brute force %zu", mode, minv, na, nb);
  free(a);
  free(b);
  tessera_c_free(q);
}

static int polls = 0, stop_at = 0;
static int stop_cb(void *ctx) {
  (void)ctx;
  return ++polls >= stop_at;
}

static void test_cancel(int at) {
  enum { N = 20000 };
  int64_t *k = malloc(sizeof(int64_t) * N), *x = malloc(sizeof(int64_t) * N);
  for (int i = 0; i < N; i++) {
    k[i] = i / 20;
    x[i] = (int64_t)(next_u64() % 3000);
  }
  tessera_query_c *q = tessera_c_new(0);
  tessera_c_add(q, k, x, NULL, N);
  polls = 0;
  stop_at = at;
  tessera_c_set_stop(q, stop_cb, NULL);
  char err[256];
  int rc = tessera_c_run(q, 0, 1, 0, -1, 4, -1, 0, err, sizeof err);
  CHECK(rc == TESSERA_C_CANCELLED, "stop at poll %d: expected TESSERA_C_CANCELLED, got %d (%s)", at, rc, err);
  int64_t a, b, vi;
  double vd;
  int g;
  CHECK(tessera_c_next(q, &a, &b, &vi, &vd, &g) == 0, "rows after a cancelled run");
  tessera_c_free(q);
  q = tessera_c_new(0);
  tessera_c_add(q, k, x, NULL, N);
  CHECK(tessera_c_run(q, 0, 1, 0, -1, 4, -1, 0, err, sizeof err) == TESSERA_C_OK, "rerun: %s", err);
  CHECK(tessera_c_next(q, &a, &b, &vi, &vd, &g) == 1, "rerun rows");
  tessera_c_free(q);
  free(k);
  free(x);
}

int main(void) {
  printf("tessera %s, API %d (header %d), core variant %s\n", tessera_c_version(), tessera_c_api_version(),
         TESSERA_API_VERSION, tessera_c_isa());
  if (tessera_c_api_version() != TESSERA_API_VERSION) return 2;
  for (int mode = 0; mode < 3; mode++)
    for (int v = 0; v < 2; v++)
      for (int batch = 0; batch < 2; batch++) {
        test_wide(mode, v, batch, 0, 0);
        test_wide(mode, v, batch, 1, 0);
      }
  test_wide(1, 0, 1, 0, 30);
  test_wide(0, 1, 0, 1, 50);
  for (int mode = 0; mode < 3; mode++)
    for (int v = 0; v < 2; v++) test_strings(mode, v);
  test_cancel(3);
  test_cancel(10);
  for (int v = 0; v < 2; v++)
    for (int batch = 0; batch < 2; batch++) {
      test_cross(v, batch, 0, 0, -1, 4000, 3000, 300, 200, 150);
      test_cross(v, batch, 1, 0, -1, 4000, 3000, 300, 200, 150);
      test_cross(v, batch, 0, 0, 256, 20000, 20000, 400, 400, 300);
    }
  test_cross(0, 1, 0, 40, -1, 20000, 20000, 400, 400, 300);
  test_cross(1, 0, 0, 150, 256, 20000, 20000, 400, 400, 300);
  test_cross_strings(0);
  test_cross_strings(1);
  for (value_kind = 1; value_kind <= 2; value_kind++) {
    const double having = value_kind == 1 ? 2e9 : 5e9;
    for (int mode = 0; mode < 3; mode++) test_wide(mode, 1, mode & 1, 0, 0);
    test_wide(1, 1, 0, 1, 0);
    test_wide(0, 1, 1, 0, having);
    test_cross(1, 0, 0, 0, -1, 4000, 3000, 300, 200, 150);
    test_cross(1, 1, 0, 0, 256, 20000, 20000, 400, 400, 300);
    test_cross(1, 0, 0, having, -1, 4000, 3000, 300, 200, 150);
  }
  value_kind = 0;
  if (wide_runs < 3) {
    fprintf(stderr, "FAIL: only %d runs of the value tests took the integer sparse path\n", wide_runs);
    fails++;
  }
  for (int mode = 0; mode < 3; mode++) test_bag(mode, -1);
  test_bag(1, 1000);
  for (int measure = 1; measure <= 4; measure++)
    for (int mode = 0; mode < 3; mode++) {
      test_similarity(measure, 0.3, mode, 0, 0, -1);
      test_similarity(measure, 0.6, mode, 0, 0, 256);
      test_similarity(measure, 0.2, mode, 1, 1, -1);
      test_similarity_nested(measure, 0.3, mode, -1);
      test_similarity_nested(measure, 0.7, mode, 64);
    }
  printf("%s (%d failures)\n", fails ? "FAILED" : "all passed", fails);
  return fails ? 1 : 0;
}
