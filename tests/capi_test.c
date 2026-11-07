#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tessera_c.h"

static int fails = 0;
static volatile double zero_double = 0.0;
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
      if (mode == 3 && x[a] == x[b]) continue;
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
    if (mode == 3 && b[i].x0 == b[i].x1) continue;
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
    if (mode == 3 && b[i].x0 == b[i].x1) continue;
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

static void test_distinct_keys(int mode, int cross, double minv) {
  enum { N = 3000 };
  static int64_t k[N], x[N], dk[N], dx[N];
  static double v[N];
  size_t nd = 0;
  for (int i = 0; i < N; i++) {
    k[i] = (int64_t)(next_u64() % (i < 2000 ? 3 : 60));
    x[i] = i < 2000 ? (int64_t)(next_u64() % 3) : 3 + (int64_t)(next_u64() % 50);
    v[i] = 1;
    int seen = 0;
    for (size_t j = 0; j < nd && !seen; j++) seen = dk[j] == k[i] && dx[j] == x[i];
    if (!seen) {
      dk[nd] = k[i];
      dx[nd] = x[i];
      nd++;
    }
  }
  tessera_query_c *q = cross ? tessera_c_new_cross(0) : tessera_c_new(0);
  CHECK(q && tessera_c_add(q, k, x, NULL, N) == TESSERA_C_OK, "distinct add");
  if (cross) CHECK(tessera_c_add_y(q, k, x, NULL, N) == TESSERA_C_OK, "distinct add_y");
  tessera_c_set_distinct_keys(q, 1);
  char err[256];
  CHECK(tessera_c_run(q, mode, 1, (uint64_t)(minv > 0 ? minv : 0), -1, 4, -1, 0, err, sizeof err) == TESSERA_C_OK,
        "distinct run: %s", err);
  Row *a, *b;
  size_t na = collect(q, 1, minv, &a), nb = brute(dk, dx, NULL, nd, cross ? 0 : mode, minv, &b);
  CHECK(same(a, na, b, nb), "distinct keys mode %d cross %d having %g: %zu rows vs brute force %zu", mode, cross, minv, na,
        nb);
  free(a);
  free(b);
  tessera_c_free(q);
  q = tessera_c_new(1);
  tessera_c_add(q, k, x, v, N);
  tessera_c_set_distinct_keys(q, 1);
  CHECK(tessera_c_run(q, mode, 1, 0, -1, 4, -1, 0, err, sizeof err) == TESSERA_C_ERROR, "distinct keys with values ran");
  tessera_c_free(q);
}

static void test_two(int mode, int cross, int frac, double minc) {
  enum { N = 2500 };
  static int64_t k[N], x[N];
  static double v[N];
  for (int i = 0; i < N; i++) {
    k[i] = (int64_t)(next_u64() % 300);
    x[i] = (int64_t)((next_u64() % 60) * (next_u64() % 60) / 59);
    v[i] = frac ? 0.25 * (double)(1 + next_u64() % 9) - 1.0 : (double)(1 + next_u64() % 5);
  }
  tessera_query_c *q = cross ? tessera_c_new_cross(1) : tessera_c_new(1);
  CHECK(q && tessera_c_add(q, k, x, v, N) == TESSERA_C_OK, "two add");
  if (cross) CHECK(tessera_c_add_y(q, k, x, v, N) == TESSERA_C_OK, "two add_y");
  tessera_c_set_two(q, 1);
  char err[256];
  CHECK(tessera_c_run(q, mode, 1, (uint64_t)(minc > 0 ? minc : 0), -1, 4, -1, 0, err, sizeof err) == TESSERA_C_OK,
        "two run: %s", err);
  Row *bc, *bs;
  const int bm = cross ? 0 : mode;
  size_t nc = brute(k, x, NULL, N, bm, minc, &bc), ns = brute(k, x, v, N, bm, -1e300, &bs);
  size_t m = 0, bad = 0;
  int64_t x0[5], x1[5], cnt[5], hi[5];
  uint64_t lo[5];
  double vd[5];
  for (;;) {
    size_t got = tessera_c_next_batch2(q, x0, x1, cnt, hi, lo, vd, 5);
    if (!got) break;
    for (size_t i = 0; i < got; i++) {
      if ((double)cnt[i] < minc) continue;
      size_t a = 0, b = nc;
      while (a < b) {
        size_t c = (a + b) / 2;
        if (bc[c].x0 < x0[i] || (bc[c].x0 == x0[i] && bc[c].x1 < x1[i])) a = c + 1;
        else b = c;
      }
      size_t sa = 0, sb = ns;
      while (sa < sb) {
        size_t c = (sa + sb) / 2;
        if (bs[c].x0 < x0[i] || (bs[c].x0 == x0[i] && bs[c].x1 < x1[i])) sa = c + 1;
        else sb = c;
      }
      if (a == nc || bc[a].x0 != x0[i] || bc[a].x1 != x1[i] || bc[a].v != (double)cnt[i] || sa == ns ||
          bs[sa].x0 != x0[i] || bs[sa].x1 != x1[i] || fabs(bs[sa].v - vd[i]) > 1e-9 * (1 + fabs(vd[i])) ||
          (!frac && (hi[i] != 0 || (double)lo[i] != bs[sa].v)))
        bad++;
      m++;
    }
  }
  CHECK(tessera_c_general2(q) == frac, "two: general2 %d for frac %d", tessera_c_general2(q), frac);
  CHECK(bad == 0 && m == nc, "two mode %d cross %d frac %d having %g: %zu rows (brute force %zu), %zu wrong", mode, cross,
        frac, minc, m, nc, bad);
  free(bc);
  free(bs);
  tessera_c_free(q);
}

static void test_dict_order(void) {
  enum { N = 3000, NS = 12 };
  const char *pool[NS] = {"a", "B", "apple", "Apple", "b", "banana", "z", "Zed", "zz", "m", "M", "mango"};
  int64_t k[N], x[N];
  tessera_dict_c *d1 = tessera_dict_new(), *d2 = tessera_dict_new();
  int64_t x2[N];
  for (int i = 0; i < N; i++) {
    const char *t = pool[(next_u64() % NS) * (next_u64() % NS) / (NS - 1)];
    k[i] = (int64_t)(next_u64() % 400);
    x[i] = tessera_dict_code(d1, t, strlen(t));
    x2[i] = tessera_dict_code(d2, t, strlen(t));
  }
  const size_t n = tessera_dict_size(d1);
  int64_t rank[NS];
  for (size_t c = 0; c < n; c++) {
    size_t lc, lo;
    const char *sc = tessera_dict_str(d1, (int64_t)c, &lc);
    rank[c] = 0;
    for (size_t o = 0; o < n; o++) {
      const char *so = tessera_dict_str(d1, (int64_t)o, &lo);
      const int m = memcmp(so, sc, lo < lc ? lo : lc);
      rank[c] += m > 0 || (m == 0 && lo > lc);
    }
  }
  int64_t bad_rank[NS];
  memcpy(bad_rank, rank, sizeof rank);
  bad_rank[0] = bad_rank[1];
  CHECK(tessera_dict_set_order(d1, bad_rank, n) == -1, "dict order: a non-permutation accepted");
  CHECK(tessera_dict_set_order(d1, rank, n) == 0, "dict order: set_order failed");
  tessera_query_c *q1 = tessera_c_new(0), *q2 = tessera_c_new(0);
  CHECK(q1 && q2 && tessera_c_add(q1, k, x, NULL, N) == 0 && tessera_c_add(q2, k, x2, NULL, N) == 0, "dict order add");
  tessera_c_set_xdict(q1, d1);
  tessera_c_set_xdict(q2, d2);
  char err[256];
  CHECK(tessera_c_run(q1, 1, 1, 0, -1, 4, -1, 0, err, sizeof err) == 0 && tessera_c_run(q2, 1, 1, 0, -1, 4, -1, 0, err, sizeof err) == 0,
        "dict order run: %s", err);
  int64_t a0, a1, vi;
  double vd;
  int gen;
  long n1 = 0, n2 = 0, sum1 = 0, sum2 = 0, wrong = 0;
  while (tessera_c_next(q1, &a0, &a1, &vi, &vd, &gen)) {
    size_t l0, l1;
    const char *s0 = tessera_c_xstr(q1, a0, &l0), *s1 = tessera_c_xstr(q1, a1, &l1);
    const int m = memcmp(s0, s1, l0 < l1 ? l0 : l1);
    wrong += !(m > 0 || (m == 0 && l0 > l1));
    n1++;
    sum1 += vi;
  }
  while (tessera_c_next(q2, &a0, &a1, &vi, &vd, &gen)) {
    n2++;
    sum2 += vi;
  }
  CHECK(wrong == 0 && n1 == n2 && sum1 == sum2 && n1 > 0, "dict order: %ld wrong orientations, rows %ld vs %ld, sums %ld vs %ld",
        wrong, n1, n2, sum1, sum2);
  tessera_c_free(q1);
  tessera_c_free(q2);
  tessera_dict_free(d1);
  tessera_dict_free(d2);
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

static int filter_none(void *ctx, int64_t a, int64_t b) {
  (void)ctx; (void)a; (void)b;
  return 0;
}

static int run_float(tessera_query_c *q, int mode, int64_t topk) {
  char err[256];
  const int rc = tessera_c_run(q, mode, 1, 0, topk, 2, -1, 0, err, sizeof err);
  if (rc != TESSERA_C_OK) {
    fprintf(stderr, "FAIL: float values: run failed: %s\n", err);
    fails++;
  }
  return tessera_c_float_errors(q);
}

static size_t rows_of(tessera_query_c *q, int64_t *a, int64_t *b, double *v, size_t cap) {
  int64_t vi[64];
  double vd[64];
  size_t n = tessera_c_next_batch(q, a, b, vi, vd, cap < 64 ? cap : 64);
  for (size_t i = 0; i < n; i++) v[i] = tessera_c_general(q) ? vd[i] : (double)vi[i];
  return n;
}

static double cell_of(const int64_t *a, const int64_t *b, const double *v, size_t n, int64_t x, int64_t y) {
  for (size_t i = 0; i < n; i++)
    if (a[i] == x && b[i] == y) return v[i];
  return -12345;
}

static void test_float_values(void) {
  int64_t a[64], b[64];
  double v[64];
  const double inf = 1.0 / zero_double, nan_v = inf - inf;
  {
    int64_t rk[] = {1, 2, 3}, rx[] = {1, 2, 3}, sk[] = {1, 2, 3}, sy[] = {1, 1, 1};
    double rv[] = {5, -1e308, 1}, sv[] = {5, 10, 1};
    tessera_query_c *q = tessera_c_new_cross(1);
    tessera_c_add(q, rk, rx, rv, 3);
    tessera_c_add_y(q, sk, sy, sv, 3);
    tessera_c_set_float_errors(q, 1);
    CHECK(run_float(q, 0, 1) == 1, "float values: the low cell's overflow is not reported");
    tessera_c_free(q);
    q = tessera_c_new_cross(1);
    tessera_c_add(q, rk, rx, rv, 3);
    tessera_c_add_y(q, sk, sy, sv, 3);
    CHECK(run_float(q, 0, 1) == 0, "float values: errors reported without being asked for");
    tessera_c_free(q);
  }
  {
    int64_t rk[] = {1, 2}, rx[] = {1, 1}, sk[] = {1, 2}, sy[] = {1, 2};
    double rv[] = {1e-200, 1}, sv[] = {1e-200, 1};
    for (int drop = 0; drop < 2; drop++) {
      tessera_query_c *q = tessera_c_new_cross(1);
      tessera_c_add(q, rk, rx, rv, 2);
      tessera_c_add_y(q, sk, sy, sv, 2);
      tessera_c_set_float_errors(q, 1);
      if (drop) tessera_c_set_pair_filter(q, filter_none, NULL, 64);
      const int e = run_float(q, 0, -1);
      CHECK(e == (drop ? 0 : 2), "float values: underflow reported as %d with the filter %d", e, drop);
      tessera_c_free(q);
    }
  }
  {
    int64_t rk[] = {1, 2, 3}, rx[] = {1, 1, 2}, sk[] = {1, 2, 3}, sy[] = {1, 1, 1};
    double rv[] = {1.3e154, 1.3e154, inf}, sv[] = {1.3e154, 1.3e154, 1};
    for (int drop = 0; drop < 2; drop++) {
      tessera_query_c *q = tessera_c_new_cross(1);
      tessera_c_add(q, rk, rx, rv, 3);
      tessera_c_add_y(q, sk, sy, sv, 3);
      tessera_c_set_float_errors(q, 1);
      if (drop) tessera_c_set_pair_filter(q, filter_none, NULL, 64);
      const int e = run_float(q, 0, -1);
      CHECK(e == (drop ? 0 : 1), "float values: a partial sum's overflow reported as %d with the filter %d", e, drop);
      tessera_c_free(q);
    }
  }
  {
    int64_t rk[] = {1, 2}, rx[] = {1, 1}, sk[] = {1, 2}, sy[] = {1, 1};
    double rv[] = {2e77, -2e77}, same[] = {2e77, 2e77}, sv[] = {2e77, 2e77};
    for (int accum = 0; accum < 2; accum++) {
      for (int equal = 0; equal < 2; equal++) {
        tessera_query_c *q = tessera_c_new_cross(1);
        tessera_c_add(q, rk, rx, equal ? same : rv, 2);
        tessera_c_add_y(q, sk, sy, sv, 2);
        tessera_c_set_float_errors(q, 1);
        tessera_c_set_float_accum(q, accum);
        const int e = run_float(q, 0, -1);
        CHECK(e == (accum && !equal ? 1 : 0), "float values: accumulating %d, equal products %d, reported %d", accum, equal, e);
        if (e == 0) {
          const size_t n = rows_of(q, a, b, v, 64);
          CHECK(cell_of(a, b, v, n, 1, 1) == (equal ? 2 * (2e77 * 2e77) : 0), "float values: accumulating %d, equal products %d, wrong sum", accum, equal);
        }
        tessera_c_free(q);
      }
    }
  }
  {
    int64_t rk[] = {1, 1, 1, 2, 2}, rx[] = {1, 2, 3, 1, 2};
    double rv[] = {1.5, 2.5, 0.5, 1.5, 2.5};
    tessera_query_c *q = tessera_c_new(1);
    tessera_c_add(q, rk, rx, rv, 5);
    tessera_c_set_float_errors(q, 1);
    tessera_c_set_float_accum(q, 1);
    CHECK(run_float(q, 1, 1) == 0, "float values: an accumulation error on ordinary values");
    CHECK(rows_of(q, a, b, v, 64) == 1, "float values: the top-k did not prune with accumulation on");
    tessera_c_free(q);
  }
  {
    int64_t rk[] = {1, 1, 1, 2, 2}, rx[] = {1, 2, 3, 1, 2};
    double rv[] = {1.5, 2.5, 0.5, 1.5, 2.5};
    tessera_query_c *q = tessera_c_new(1);
    tessera_c_add(q, rk, rx, rv, 5);
    tessera_c_set_float_errors(q, 1);
    CHECK(run_float(q, 1, 1) == 0, "float values: an error on ordinary values");
    CHECK(rows_of(q, a, b, v, 64) == 1, "float values: the top-k did not prune ordinary values");
    tessera_c_free(q);
  }
  {
    int64_t rk[] = {1, 1, 1}, rx[] = {1, 2, 2};
    double rv[] = {inf, 3, -7};
    for (int mode = 0; mode < 4; mode++) {
      tessera_query_c *q = tessera_c_new(1);
      tessera_c_add(q, rk, rx, rv, 3);
      CHECK(run_float(q, mode, -1) == 0, "float values: repeated rows, mode %d", mode);
      const size_t n = rows_of(q, a, b, v, 64);
      const double c = cell_of(a, b, v, n, 1, 2);
      CHECK(c != c, "float values: repeated Infinity in mode %d gives %g, not NaN", mode, c);
      tessera_c_free(q);
    }
    int64_t sk[] = {1, 1}, sy[] = {1, 1};
    double sv[] = {3, -7}, xv[] = {inf};
    tessera_query_c *q = tessera_c_new_cross(1);
    tessera_c_add(q, rk, rx, xv, 1);
    tessera_c_add_y(q, sk, sy, sv, 2);
    run_float(q, 0, -1);
    size_t n = rows_of(q, a, b, v, 64);
    CHECK(n == 1 && v[0] != v[0], "float values: repeated rows of the cross product");
    tessera_c_free(q);
  }
  {
    int64_t rk[] = {1, 2}, rx[] = {1, 1}, sk[] = {1, 2}, sy[] = {1, 1};
    double rv[] = {1e308, 1e154}, sv[] = {-1e308, 1e308};
    tessera_query_c *q = tessera_c_new_cross(1);
    tessera_c_add(q, rk, rx, rv, 2);
    tessera_c_add_y(q, sk, sy, sv, 2);
    run_float(q, 0, -1);
    size_t n = rows_of(q, a, b, v, 64);
    CHECK(n == 1 && v[0] != v[0], "float values: opposite overflowing products give %g", n ? v[0] : 0.0);
    tessera_c_free(q);
  }
  {
    int64_t rk[] = {1, 1, 1, 2, 2}, rx[] = {1, 2, 3, 1, 2}, rv[] = {5, 5, 1, 5, 5};
    for (int reach = 0; reach < 2; reach++) {
      tessera_query_c *q = tessera_c_new(1);
      tessera_c_add_i64(q, rk, rx, rv, 5);
      tessera_c_set_sum_limit(q, reach ? 10 : 1e12);
      run_float(q, 1, 1);
      const size_t n = rows_of(q, a, b, v, 64);
      CHECK(reach ? n == 3 : n == 1, "float values: %zu rows with the sum limit %s", n, reach ? "reached" : "far");
      tessera_c_free(q);
    }
  }
  {
    union { uint64_t u; double d; } under, over;
    under.u = TESSERA_C_ROW_VALUE_UNDERFLOW;
    over.u = TESSERA_C_ROW_VALUE_OVERFLOW;
    int64_t rk[] = {1, 2}, rx[] = {1, 2}, sk[] = {1, 2}, sy[] = {1, 2};
    const double cases[][4] = {{under.d, 1, 1, 1}, {over.d, 1, 1, 1}, {1e308, 1, 10, 1}, {1e-200, 1, 1e-200, 1}, {2, 3, 4, 5}};
    const int want[] = {2, 1, 1, 2, 0};
    for (int c = 0; c < 5; c++) {
      tessera_query_c *q = tessera_c_new_cross(0);
      tessera_c_set_many(q, 1, 0);
      tessera_c_set_float_errors(q, 1);
      tessera_c_add_many(q, 0, rk, rx, cases[c], 2);
      tessera_c_add_many(q, 1, sk, sy, cases[c] + 2, 2);
      const int e = run_float(q, 0, -1);
      CHECK(e == want[c], "float values: several sums, case %d: %d", c, e);
      tessera_c_free(q);
    }
    int64_t alone[] = {9, 2};
    const double marked[] = {under.d, 1}, other[] = {1, 1};
    tessera_query_c *q = tessera_c_new_cross(0);
    tessera_c_set_many(q, 1, 0);
    tessera_c_set_float_errors(q, 1);
    tessera_c_add_many(q, 0, alone, rx, marked, 2);
    tessera_c_add_many(q, 1, sk, sy, other, 2);
    CHECK(run_float(q, 0, -1) == 0, "float values: a marked row that joins nothing was reported");
    tessera_c_free(q);
  }
}

static void test_null_sums(int pair_op, int threads) {
  enum { NR = 3000, NS = 2000, NK = 200, NX = 40, NY = 30, M = 4 };
  static int64_t rk[NR], rx[NR], ri[NR], sk[NS], sy[NS], sj[NS];
  static double rv[NR], sw[NS];
  static uint64_t rn[NR], sn[NS];
  static double rd[NR * M], sd[NS * M];
  static int64_t rdi[NR * M], sdi[NS * M];
  for (int r = 0; r < NR; r++) {
    rk[r] = (int64_t)(next_u64() % NK);
    rx[r] = (int64_t)(next_u64() % NX);
    if (r < 1000 && rx[r] == 7) rx[r] = 8;
    rv[r] = (double)((int)(next_u64() % 9) - 3);
    ri[r] = (int64_t)(next_u64() % 4294967296ull) - 2147483648ll;
    const int vnull = r >= 1000 && next_u64() % 3 == 0, inull = r >= 1000 && next_u64() % 4 == 0;
    rn[r] = (vnull ? 1u | 4u | 8u : 0u) | (inull ? 2u : 0u);
    if (rx[r] == 7) rn[r] |= 1u | 4u | 8u;
  }
  for (int r = 0; r < NS; r++) {
    sk[r] = (int64_t)(next_u64() % NK);
    sy[r] = (int64_t)(next_u64() % NY);
    if (r >= 1500 && sy[r] == 5) sy[r] = 6;
    sw[r] = (double)(1 + (int)(next_u64() % 4));
    sj[r] = (int64_t)(next_u64() % 4294967296ull) - 2147483648ll;
    const int wnull = r < 1500 && next_u64() % 3 == 0, jnull = r < 1500 && next_u64() % 5 == 0;
    sn[r] = (wnull ? 1u | 8u : 0u) | (jnull ? 2u : 0u);
    if (sy[r] == 5) sn[r] |= 2u;
  }
  for (int r = 0; r < NR; r++) {
    rd[r * M + 0] = rn[r] & 1 ? 0 : rv[r];
    rdi[r * M + 1] = rn[r] & 2 ? 0 : ri[r];
    rd[r * M + 2] = rn[r] & 4 ? 0 : rv[r];
    rd[r * M + 3] = 1;
  }
  for (int r = 0; r < NS; r++) {
    sd[r * M + 0] = sn[r] & 1 ? 0 : sw[r];
    sdi[r * M + 1] = sn[r] & 2 ? 0 : sj[r];
    sd[r * M + 2] = 1;
    sd[r * M + 3] = 1;
  }
  static int64_t cnt[NX][NY], vcnt[NX][NY][M];
  static double dsum[NX][NY][M];
  static __int128 isum[NX][NY];
  __int128 pmin = 0, pmax = 0, cmin = 0, cmax = 0;
  int have_p = 0, have_c = 0;
  memset(cnt, 0, sizeof cnt);
  memset(vcnt, 0, sizeof vcnt);
  memset(dsum, 0, sizeof dsum);
  memset(isum, 0, sizeof isum);
  for (int a = 0; a < NR; a++) {
    for (int b = 0; b < NS; b++) {
      if (rk[a] != sk[b]) continue;
      const int64_t xv = rx[a], yv = sy[b];
      const int pass = pair_op == 0 ? 1 : pair_op == 1 ? xv < yv : pair_op == 2 ? xv <= yv : pair_op == 3 ? xv > yv
                       : pair_op == 4 ? xv >= yv : xv != yv;
      if (!pass) continue;
      cnt[xv][yv]++;
      const uint64_t skip = rn[a] | sn[b];
      for (int m = 0; m < M; m++) {
        if (skip >> m & 1) continue;
        vcnt[xv][yv][m]++;
        if (m == 1) {
          const __int128 pr = (__int128)ri[a] * sj[b];
          isum[xv][yv] += pr;
          if (!have_p || pr < pmin) pmin = pr;
          if (!have_p || pr > pmax) pmax = pr;
          have_p = 1;
        } else {
          dsum[xv][yv][m] += rd[a * M + m] * sd[b * M + m];
        }
      }
    }
  }
  int ref_cells = 0;
  for (int x = 0; x < NX; x++) {
    for (int y = 0; y < NY; y++) {
      if (cnt[x][y]) ref_cells++;
      if (cnt[x][y] && vcnt[x][y][1]) {
        if (!have_c || isum[x][y] < cmin) cmin = isum[x][y];
        if (!have_c || isum[x][y] > cmax) cmax = isum[x][y];
        have_c = 1;
      }
    }
  }
  tessera_query_c *q = tessera_c_new_cross(0);
  tessera_c_set_many(q, M, pair_op);
  tessera_c_set_many_int(q, 1, 1);
  CHECK(tessera_c_add_many(q, 0, rk, rx, rd, 1) == -1, "null sums: tessera_c_add_many took an integer sum");
  CHECK(tessera_c_add_many2(q, 0, rk, rx, rd, rdi, NULL, 1000) == 0, "null sums: add without marks");
  CHECK(tessera_c_add_many2(q, 0, rk + 1000, rx + 1000, rd + 1000 * M, rdi + 1000 * M, rn + 1000, NR - 1000) == 0,
        "null sums: add with marks");
  CHECK(tessera_c_add_many2(q, 1, sk, sy, sd, sdi, sn, 1500) == 0, "null sums: second input with marks");
  CHECK(tessera_c_add_many2(q, 1, sk + 1500, sy + 1500, sd + 1500 * M, sdi + 1500 * M, NULL, NS - 1500) == 0,
        "null sums: second input without marks");
  char err[256];
  CHECK(tessera_c_run(q, 0, 1, 0, -1, threads, -1, 0, err, sizeof err) == TESSERA_C_OK, "null sums: run: %s", err);
  int64_t x0[64], x1[64], count[64], isums[64 * M * 2];
  double sums[64 * M];
  uint64_t nulls[64];
  int got = 0, nulled[M] = {0, 0, 0, 0};
  size_t n;
  while ((n = tessera_c_next_many2(q, x0, x1, count, sums, isums, nulls, 64)) > 0) {
    for (size_t r = 0; r < n; r++) {
      const int64_t x = x0[r], y = x1[r];
      CHECK(x >= 0 && x < NX && y >= 0 && y < NY && cnt[x][y] == count[r], "null sums: cell (%lld, %lld) count %lld",
            (long long)x, (long long)y, (long long)count[r]);
      for (int m = 0; m < M; m++) {
        const int is_null = (int)(nulls[r] >> m & 1);
        CHECK(is_null == (vcnt[x][y][m] == 0), "null sums op %d: cell (%lld, %lld) sum %d NULL %d, valid pairs %lld",
              pair_op, (long long)x, (long long)y, m, is_null, (long long)vcnt[x][y][m]);
        nulled[m] += is_null;
        if (is_null) continue;
        if (m == 1) {
          const __int128 v = (__int128)(((unsigned __int128)(uint64_t)isums[2 * (r * M + 1) + 1] << 64) |
                                        (uint64_t)isums[2 * (r * M + 1)]);
          CHECK(v == isum[x][y], "null sums: cell (%lld, %lld) integer sum", (long long)x, (long long)y);
        } else {
          CHECK(sums[r * M + m] == dsum[x][y][m], "null sums: cell (%lld, %lld) sum %d = %g, want %g", (long long)x,
                (long long)y, m, sums[r * M + m], dsum[x][y][m]);
        }
      }
      CHECK(sums[r * M + 3] == (double)vcnt[x][y][0], "null sums: divisor of cell (%lld, %lld)", (long long)x,
            (long long)y);
      got++;
    }
  }
  CHECK(got == ref_cells, "null sums op %d: %d cells, want %d", pair_op, got, ref_cells);
  CHECK(pair_op != 0 || (nulled[0] > 0 && nulled[1] > 0 && nulled[2] > 0), "null sums: no NULL cell to check");
  const int want = (have_p && (pmin < INT64_MIN || pmax > INT64_MAX) ? 1 : 0) |
                   (have_c && (cmin < INT64_MIN || cmax > INT64_MAX) ? 2 : 0);
  CHECK(tessera_c_many_overflow(q, 1, 64, 64) == want, "null sums: overflow flags %d, want %d",
        tessera_c_many_overflow(q, 1, 64, 64), want);
  CHECK(pair_op != 0 || want == 2, "null sums: the integer sums do not leave 64 bits");
  tessera_c_free(q);
}

static void test_null_float_errors(void) {
  union { uint64_t u; double d; } over;
  over.u = TESSERA_C_ROW_VALUE_OVERFLOW;
  int64_t rk[] = {1, 2}, rx[] = {1, 2}, sk[] = {1, 2}, sy[] = {1, 2};
  for (int c = 0; c < 5; c++) {
    const double rv[] = {c < 3 ? over.d : 1e200, 1}, sv[] = {c < 3 ? 1 : 1e200, 1};
    const uint64_t rnull[] = {c == 1 ? 1u : 0u, 0}, snull[] = {c == 2 || c == 4 ? 1u : 0u, 0};
    const int want = c == 4 ? 0 : 1;
    tessera_query_c *q = tessera_c_new_cross(0);
    tessera_c_set_many(q, 1, 0);
    tessera_c_set_float_errors(q, 1);
    tessera_c_add_many2(q, 0, rk, rx, rv, NULL, rnull, 2);
    tessera_c_add_many2(q, 1, sk, sy, sv, NULL, snull, 2);
    char err[256];
    CHECK(tessera_c_run(q, 0, 1, 0, -1, 2, -1, 0, err, sizeof err) == TESSERA_C_OK, "null float errors: %s", err);
    CHECK(tessera_c_float_errors(q) == want, "null float errors: case %d reported %d", c, tessera_c_float_errors(q));
    int64_t x0[4], x1[4], count[4];
    double sums[4];
    uint64_t nulls[4];
    size_t n = tessera_c_next_many2(q, x0, x1, count, sums, NULL, nulls, 4);
    int nulled = 0;
    for (size_t r = 0; r < n; r++) nulled += x0[r] == 1 && nulls[r] == 1;
    CHECK(n == 2 && nulled == (c == 1 || c == 2 || c == 4 ? 1 : 0), "null float errors: case %d: %zu rows, %d NULL", c, n,
          nulled);
    tessera_c_free(q);
  }
}

static void test_many_edges(void) {
  enum { M = 64, M2 = 65 };
  {
    int64_t rk[] = {1, 1, 1}, rx[] = {1, 1, 2}, sk[] = {1}, sy[] = {1};
    static double rv[3 * M], sv[M];
    for (int i = 0; i < 3 * M; i++) rv[i] = 1;
    for (int i = 0; i < M; i++) sv[i] = 2;
    const uint64_t rn[] = {1ull << 63, 0, 1ull << 63}, sn[] = {0};
    tessera_query_c *q = tessera_c_new_cross(0);
    tessera_c_set_many(q, M, 0);
    CHECK(tessera_c_add_many2(q, 0, rk, rx, rv, NULL, rn, 3) == 0 && tessera_c_add_many2(q, 1, sk, sy, sv, NULL, sn, 1) == 0,
          "many edges: add with 64 sums");
    char err[256];
    CHECK(tessera_c_run(q, 0, 1, 0, -1, 2, -1, 0, err, sizeof err) == TESSERA_C_OK, "many edges: 64 sums: %s", err);
    int64_t x0[4], x1[4], c[4];
    static double sums[4 * M];
    uint64_t nl[4];
    const size_t n = tessera_c_next_many2(q, x0, x1, c, sums, NULL, nl, 4);
    int good = n == 2;
    for (size_t r = 0; r < n && good; r++) {
      const int one = x0[r] == 1;
      good = c[r] == (one ? 2 : 1) && sums[r * M] == (one ? 4 : 2) && sums[r * M + 63] == (one ? 2 : 0) &&
             nl[r] == (one ? 0 : 1ull << 63);
    }
    CHECK(good, "many edges: NULL marks on sum 63");
    tessera_c_free(q);
  }
  {
    int64_t rk[] = {1}, rx[] = {1}, sk[] = {1}, sy[] = {1};
    static double rv[M2], sv[M2];
    for (int i = 0; i < M2; i++) rv[i] = i, sv[i] = 1;
    tessera_query_c *q = tessera_c_new_cross(0);
    tessera_c_set_many(q, M2, 0);
    tessera_c_add_many(q, 0, rk, rx, rv, 1);
    tessera_c_add_many(q, 1, sk, sy, sv, 1);
    char err[256];
    CHECK(tessera_c_run(q, 0, 1, 0, -1, 1, -1, 0, err, sizeof err) == TESSERA_C_OK, "many edges: 65 sums: %s", err);
    int64_t x0[2], x1[2], c[2];
    static double sums[2 * M2];
    const size_t n = tessera_c_next_many(q, x0, x1, c, sums, 2);
    CHECK(n == 1 && sums[64] == 64, "many edges: 65 sums without marks");
    tessera_c_free(q);
  }
  {
    int64_t rk[] = {1, 1}, rx[] = {1, 2}, sk[] = {1}, sy[] = {1};
    const double rv[] = {3, 4};
    const int64_t sv[] = {5};
    tessera_query_c *q = tessera_c_new_cross(0);
    tessera_c_set_many(q, 1, 0);
    CHECK(tessera_c_add_many2(q, 0, rk, rx, rv, NULL, NULL, 2) == 0, "many edges: first batch");
    tessera_c_set_many_int(q, 0, 1);
    CHECK(tessera_c_add_many2(q, 1, sk, sy, NULL, sv, NULL, 1) == -1, "many edges: set_many_int after rows took effect");
    char err[256];
    CHECK(tessera_c_run(q, 0, 1, 0, -1, 1, -1, 0, err, sizeof err) == TESSERA_C_OK, "many edges: run after a refused batch: %s",
          err);
    tessera_c_free(q);
  }
  {
    int64_t k[1] = {0}, x[1] = {0};
    tessera_query_c *q = tessera_c_new_cross(0);
    tessera_c_set_many(q, 2, 0);
    CHECK(tessera_c_add_many(q, 0, k, x, NULL, 0) == 0, "many edges: an empty batch without values");
    tessera_c_free(q);
  }
}

int main(void) {
  printf("tessera %s, API %d (header %d), core variant %s\n", tessera_c_version(), tessera_c_api_version(),
         TESSERA_API_VERSION, tessera_c_isa());
  if (tessera_c_api_version() != TESSERA_API_VERSION) return 2;
  for (int mode = 0; mode < 4; mode++)
    for (int v = 0; v < 2; v++)
      for (int batch = 0; batch < 2; batch++) {
        test_wide(mode, v, batch, 0, 0);
        test_wide(mode, v, batch, 1, 0);
      }
  test_wide(1, 0, 1, 0, 30);
  test_wide(0, 1, 0, 1, 50);
  for (int mode = 0; mode < 4; mode++)
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
    for (int mode = 0; mode < 4; mode++) test_wide(mode, 1, mode & 1, 0, 0);
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
  for (int mode = 0; mode < 4; mode++) test_bag(mode, -1);
  test_bag(1, 1000);
  test_bag(3, 1000);
  for (int mode = 0; mode < 4; mode++) test_distinct_keys(mode, 0, -1);
  test_distinct_keys(1, 0, 20);
  test_distinct_keys(0, 1, -1);
  test_distinct_keys(0, 1, 20);
  test_dict_order();
  for (int mode = 0; mode < 4; mode++)
    for (int frac = 0; frac < 2; frac++) test_two(mode, 0, frac, -1);
  test_two(1, 0, 0, 30);
  test_two(0, 1, 0, -1);
  test_two(0, 1, 1, -1);
  test_two(0, 1, 0, 30);
  for (int measure = 1; measure <= 4; measure++)
    for (int mode = 0; mode < 4; mode++) {
      test_similarity(measure, 0.3, mode, 0, 0, -1);
      test_similarity(measure, 0.6, mode, 0, 0, 256);
      test_similarity(measure, 0.2, mode, 1, 1, -1);
      test_similarity_nested(measure, 0.3, mode, -1);
      test_similarity_nested(measure, 0.7, mode, 64);
    }
  test_float_values();
  for (int op = 0; op <= 5; op++) test_null_sums(op, op == 0 ? 1 : 4);
  test_null_float_errors();
  test_many_edges();
  printf("%s (%d failures)\n", fails ? "FAILED" : "all passed", fails);
  return fails ? 1 : 0;
}
