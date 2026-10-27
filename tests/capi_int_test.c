#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tessera_c.h"

static int fails = 0;
#define CHECK(c, ...)                                      \
  do {                                                     \
    if (!(c)) {                                            \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
      fprintf(stderr, __VA_ARGS__);                        \
      fprintf(stderr, "\n");                               \
      fails++;                                             \
      return;                                              \
    }                                                      \
  } while (0)

typedef __int128 i128;

static i128 value_of(tessera_query_c *q, int64_t a, int64_t b, int *found) {
  int64_t x0[64], x1[64], hi[64];
  uint64_t lo[64];
  size_t n;
  *found = 0;
  tessera_c_rewind(q);
  while ((n = tessera_c_next_batch128(q, x0, x1, hi, lo, 64)) > 0)
    for (size_t r = 0; r < n; r++)
      if (x0[r] == a && x1[r] == b) {
        *found = 1;
        return (i128)(((unsigned __int128)(uint64_t)hi[r] << 64) | lo[r]);
      }
  return 0;
}

static tessera_query_c *run(const int64_t *k, const int64_t *x, const int64_t *v, size_t n, int mode) {
  char err[256];
  tessera_query_c *q = tessera_c_new(1);
  if (!q || tessera_c_add_i64(q, k, x, v, n) != TESSERA_C_OK) return NULL;
  if (tessera_c_run(q, mode, 1, 0, -1, 4, -1, 0, err, sizeof err) != TESSERA_C_OK) {
    fprintf(stderr, "run: %s\n", err);
    tessera_c_free(q);
    return NULL;
  }
  return q;
}

static void test_bigint_pair(void) {
  const int64_t k[] = {1, 1}, x[] = {1, 2}, v[] = {100000001, 100000001};
  tessera_query_c *q = run(k, x, v, 2, 1);
  CHECK(q, "bigint pair: run failed");
  int f;
  const i128 s = value_of(q, 1, 2, &f);
  CHECK(f && s == (i128)10000000200000001LL, "bigint pair: %lld", (long long)s);
  int64_t x0, x1, vi;
  double vd;
  tessera_c_rewind(q);
  CHECK(tessera_c_next_batch(q, &x0, &x1, &vi, &vd, 1) == 1 && vi == 10000000200000001LL, "next_batch vi %lld",
        (long long)vi);
  CHECK(tessera_c_general(q) == 0 && tessera_c_wide(q) == 0, "classes: general %d wide %d", tessera_c_general(q),
        tessera_c_wide(q));
  CHECK(tessera_c_overflow(q, 64, 64) == 0, "bigint pair: overflow %d", tessera_c_overflow(q, 64, 64));
  CHECK(tessera_c_overflow(q, 32, 0) == 1, "bigint pair: a 32-bit product must overflow");
  tessera_c_free(q);
}

static void test_signed(void) {
  const int64_t k[] = {1, 1, 2, 2, 3, 3}, x[] = {1, 2, 1, 2, 1, 3}, v[] = {5, -5, 2, 5, 0, 7};
  tessera_query_c *q = run(k, x, v, 6, 0);
  CHECK(q, "signed: run failed");
  int f;
  i128 s = value_of(q, 1, 2, &f);
  CHECK(f && s == -15, "signed (1,2): %lld", (long long)s);
  s = value_of(q, 1, 3, &f);
  CHECK(f && s == 0, "signed (1,3): found %d value %lld", f, (long long)s);
  s = value_of(q, 1, 1, &f);
  CHECK(f && s == 29, "signed (1,1): %lld", (long long)s);
  tessera_c_free(q);
}

static void test_wide_sum(void) {
  int64_t k[8], x[8], v[8];
  for (int i = 0; i < 8; i++) {
    k[i] = i / 2;
    x[i] = 1 + i % 2;
    v[i] = 1LL << 31;
  }
  tessera_query_c *q = run(k, x, v, 8, 1);
  CHECK(q, "wide sum: run failed");
  CHECK(tessera_c_wide(q) == 1, "wide sum: not wide");
  int f;
  const i128 s = value_of(q, 1, 2, &f);
  CHECK(f && s == ((i128)1 << 64), "wide sum: hi %lld lo %llu", (long long)(s >> 64), (unsigned long long)s);
  CHECK(tessera_c_overflow(q, 64, 64) == 2, "wide sum: overflow(64, 64) %d", tessera_c_overflow(q, 64, 64));
  CHECK(tessera_c_overflow(q, 64, 128) == 0, "wide sum: overflow(64, 128) %d", tessera_c_overflow(q, 64, 128));
  tessera_c_free(q);
}

static void test_merged(void) {
  const int64_t k[] = {1, 1, 1}, x[] = {1, 1, 2}, v[] = {INT64_MAX, INT64_MAX, 1};
  tessera_query_c *q = run(k, x, v, 3, 1);
  CHECK(q, "merged: run failed");
  int f;
  const i128 s = value_of(q, 1, 2, &f);
  CHECK(f && s == (i128)2 * INT64_MAX, "merged: %lld", (long long)s);
  CHECK(tessera_c_overflow(q, 64, 0) == 0, "merged: the products (INT64_MAX x 1) fit 64 bits");
  CHECK(tessera_c_overflow(q, 0, 64) == 2, "merged: the sum passes 64 bits");
  tessera_c_free(q);
}

static void test_narrow(void) {
  const int64_t k[] = {1, 1}, x[] = {1, 2}, v[] = {50000, 50000};
  tessera_query_c *q = run(k, x, v, 2, 0);
  CHECK(q, "narrow: run failed");
  CHECK(tessera_c_overflow(q, 32, 64) == 1, "narrow: overflow(32, 64) %d", tessera_c_overflow(q, 32, 64));
  CHECK(tessera_c_overflow(q, 16, 0) == 1, "narrow: overflow(16)");
  tessera_c_free(q);
  const int64_t k2[] = {1, 1}, x2[] = {1, 2}, v2[] = {50000, 1};
  q = run(k2, x2, v2, 2, 1);
  CHECK(q, "narrow lt: run failed");
  CHECK(tessera_c_overflow(q, 32, 0) == 0, "narrow lt: the self pair is not in the query");
  tessera_c_free(q);
  q = run(k2, x2, v2, 2, 2);
  CHECK(q, "narrow le: run failed");
  CHECK(tessera_c_overflow(q, 32, 0) == 1, "narrow le: the self pair 50,000^2 is in the query");
  tessera_c_free(q);
}

static void test_cross(void) {
  char err[256];
  const int64_t k[] = {1, 2}, x[] = {10, 10}, v[] = {3000000000LL, -7};
  const int64_t ky[] = {1, 2}, y[] = {20, 20}, w[] = {4000000000LL, 2};
  tessera_query_c *q = tessera_c_new_cross(1);
  CHECK(q && tessera_c_add_i64(q, k, x, v, 2) == 0 && tessera_c_add_y_i64(q, ky, y, w, 2) == 0, "cross: add");
  CHECK(tessera_c_run(q, 0, 1, 0, -1, 2, -1, 0, err, sizeof err) == TESSERA_C_OK, "cross: %s", err);
  int f;
  const i128 s = value_of(q, 10, 20, &f);
  CHECK(f && s == (i128)3000000000LL * 4000000000LL - 14, "cross: %lld", (long long)s);
  CHECK(tessera_c_overflow(q, 64, 64) == 3, "cross: overflow(64, 64) %d", tessera_c_overflow(q, 64, 64));
  CHECK(tessera_c_overflow(q, 0, 128) == 0, "cross: overflow(0, 128) %d", tessera_c_overflow(q, 0, 128));
  tessera_c_free(q);
}

static void test_mixed(void) {
  char err[256] = "";
  const int64_t k[] = {1}, x[] = {1}, vi[] = {2};
  const double vd[] = {2.5};
  tessera_query_c *q = tessera_c_new(1);
  CHECK(q && tessera_c_add_i64(q, k, x, vi, 1) == 0 && tessera_c_add(q, k, x, vd, 1) == 0, "mixed: add");
  CHECK(tessera_c_run(q, 0, 1, 0, -1, 1, -1, 0, err, sizeof err) == TESSERA_C_ERROR, "mixed: no error");
  CHECK(strstr(err, "integer and double") != NULL, "mixed: message '%s'", err);
  tessera_c_free(q);
}

int main(void) {
  printf("tessera %s, API %d (header %d), core variant %s\n", tessera_c_version(), tessera_c_api_version(),
         TESSERA_API_VERSION, tessera_c_isa());
  if (tessera_c_api_version() != TESSERA_API_VERSION) return 2;
  test_bigint_pair();
  test_signed();
  test_wide_sum();
  test_merged();
  test_narrow();
  test_cross();
  test_mixed();
  printf("%s, %d failures\n", fails ? "FAILED" : "passed", fails);
  return fails ? 1 : 0;
}
