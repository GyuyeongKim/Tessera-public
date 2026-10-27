#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "query.hpp"
#include "tessera_c.h"

namespace {

using Parts = std::vector<std::unique_ptr<tessera::QueryPart>>;
int fails = 0;

void report(const char* name, bool ok, const std::string& why = "") {
  printf("%s %s%s%s\n", name, ok ? "ok" : "FAIL", why.empty() ? "" : ": ", why.c_str());
  if (!ok) fails++;
}

Parts one_part() {
  Parts p;
  p.emplace_back(new tessera::QueryPart());
  return p;
}

void add(tessera::QueryPart& p, int64_t k, int64_t x, int64_t v) {
  p.k.push_back(k);
  p.x.push_back(x);
  p.iv.push_back(v);
}

struct Out {
  int64_t a, b;
  __int128 v;
  double d;
};
std::vector<Out> rows(const tessera::QueryResult& r, tessera::PairMode mode) {
  std::vector<Out> o;
  for (auto& c : r.out)
    for (size_t q = 0; q < tessera::cell_count(c, r.wide); q++) {
      const tessera::Triple& t = c.data[r.wide ? 2 * q : q];
      double d = 0;
      if (r.general) memcpy(&d, &t.s, sizeof d);
      const __int128 v = r.general ? 0 : tessera::cell_value128(c, q, r.wide);
      const int64_t xi = r.orig[t.i], xj = r.orig[t.j];
      if (t.i == t.j) {
        if (mode != tessera::PairMode::LT) o.push_back({xi, xi, v, d});
      } else {
        o.push_back({xi, xj, v, d});
        if (mode == tessera::PairMode::FULL) o.push_back({xj, xi, v, d});
      }
    }
  return o;
}

void topk_ival_exact() {
  const int64_t N = 8400000, A = 65535;
  const __int128 B = (__int128)N * A * A;
  int64_t r1 = 1;
  while ((__int128)(double)(int64_t)(B + r1) <= B + r1 + 1) r1++;
  Parts p = one_part();
  for (int64_t k = 0; k < N; k++)
    for (int64_t x = 0; x < 3; x++) add(*p[0], k, x, A);
  add(*p[0], N, 0, 1);
  add(*p[0], N, 1, r1);
  add(*p[0], N + 1, 0, 1);
  add(*p[0], N + 1, 2, r1 + 1);
  tessera::QuerySpec q;
  q.has_value = true;
  q.mode = tessera::PairMode::LT;
  q.topk = 1;
  q.topk_first = 2;
  tessera::QueryResult r;
  tessera::run_query(p, q, r);
  bool found = false;
  for (auto& o : rows(r, q.mode)) found = found || (o.a == 0 && o.b == 2 && o.v == B + r1 + 1);
  report("topk_ival_exact", found, found ? "" : "the top row (0, 2) is missing");
}

void topk_nonpositive() {
  Parts p = one_part();
  for (int64_t k = 0; k < 100; k++) {
    add(*p[0], k, k % 10, -(k + 1));
    add(*p[0], k, (k + 3) % 10, 1);
  }
  tessera::QuerySpec q;
  q.has_value = true;
  q.mode = tessera::PairMode::LT;
  q.topk = 3;
  tessera::QueryResult r;
  tessera::run_query(p, q, r);
  const auto o = rows(r, q.mode);
  std::vector<__int128> all;
  {
    __int128 s[10][10] = {};
    bool seen[10][10] = {};
    for (int64_t k = 0; k < 100; k++) {
      const int64_t a = k % 10, b = (k + 3) % 10;
      const int64_t lo = std::min(a, b), hi = std::max(a, b);
      s[lo][hi] += (__int128)(-(k + 1)) * 1;
      seen[lo][hi] = true;
    }
    for (int i = 0; i < 10; i++)
      for (int j = i + 1; j < 10; j++)
        if (seen[i][j]) all.push_back(s[i][j]);
  }
  std::sort(all.begin(), all.end(), std::greater<__int128>());
  const __int128 kth = all[2];
  size_t want = 0;
  for (auto v : all) want += v >= kth;
  bool ok = o.size() == want;
  for (auto& x : o) ok = ok && x.v >= kth;
  report("topk_nonpositive", ok, ok ? "" : "rows " + std::to_string(o.size()) + " want " + std::to_string(want));
}

void topk_nan() {
  Parts p = one_part();
  for (int i = 0; i < 1000; i++) {
    p[0]->k.push_back(i % 10);
    p[0]->x.push_back(i % 50);
    p[0]->v.push_back(2.0);
  }
  p[0]->k.push_back(0);
  p[0]->x.push_back(99);
  p[0]->v.push_back(std::nan(""));
  tessera::QuerySpec q;
  q.has_value = true;
  q.mode = tessera::PairMode::FULL;
  q.topk = 3;
  tessera::QueryResult r;
  tessera::run_query(p, q, r);
  size_t nan = 0, other = 0;
  for (auto& o : rows(r, q.mode)) (std::isnan(o.d) ? nan : other)++;
  report("topk_nan", nan == 11 && other == 0, "NaN rows " + std::to_string(nan) + " (want 11), others " +
                                                 std::to_string(other));
}

void having_overflow() {
  Parts p = one_part();
  for (int64_t k = 0; k < 3; k++) {
    add(*p[0], k, 0, 1LL << 32);
    add(*p[0], k, 1, 1LL << 30);
  }
  tessera::QuerySpec q;
  q.has_value = true;
  q.mode = tessera::PairMode::LT;
  q.min_value = UINT64_MAX;
  tessera::QueryResult r;
  tessera::run_query(p, q, r);
  const int f = tessera::overflow_flags(r, 64, 64);
  report("having_overflow", f == 2, "overflow_flags(64, 64) = " + std::to_string(f) + " (want 2)");
}

void merge_class(const char* name, int64_t extra, bool small) {
  Parts p = one_part();
  add(*p[0], 0, 1, INT64_MAX);
  add(*p[0], 0, 1, -(1LL << 62) - 1);
  add(*p[0], 0, 1, 4);
  add(*p[0], 0, 1, -(1LL << 62) - 1);
  if (extra) add(*p[0], 0, 1, extra);
  add(*p[0], 0, 2, 3);
  tessera::QuerySpec q;
  q.has_value = true;
  q.mode = tessera::PairMode::FULL;
  tessera::QueryResult r;
  tessera::run_query(p, q, r);
  const __int128 m = 1 + extra;
  bool ok = !r.wide && !r.general;
  std::string why = r.wide ? "128-bit sums" : "";
  size_t n = 0;
  for (auto& o : rows(r, q.mode)) {
    const __int128 want = (o.a == 1 ? m : 3) * (o.b == 1 ? m : 3);
    ok = ok && o.v == want;
    n++;
  }
  ok = ok && n == 4;
  const __int128 lo = small ? 0 : 9, hi = small ? 0 : m * m;
  if (r.cell_min != lo || r.cell_max != hi) {
    ok = false;
    why += " extremes " + std::to_string((long long)r.cell_min) + ".." + std::to_string((long long)r.cell_max);
  }
  report(name, ok, ok ? "" : why + " rows " + std::to_string(n));
}

void part_lengths() {
  Parts p = one_part();
  for (int i = 0; i < 1000; i++) p[0]->k.push_back(i % 7);
  for (int i = 0; i < 900; i++) p[0]->x.push_back(i % 5);
  tessera::QuerySpec q;
  tessera::QueryResult r;
  std::string err;
  try {
    tessera::run_query(p, q, r);
  } catch (const std::exception& e) {
    err = e.what();
  }
  report("part_lengths", !err.empty(), err.empty() ? "no error" : "");
}

Parts big_parts() {
  Parts p = one_part();
  for (int64_t k = 0; k < 1000; k++)
    for (int64_t x = (k * 7) % 300, n = 0; n < 60; n++) {
      p[0]->k.push_back(k);
      p[0]->x.push_back((x + n * 5) % 300);
    }
  return p;
}

size_t count_cells(const tessera::QueryResult& r) {
  size_t n = 0;
  for (auto& c : r.out) n += tessera::cell_count(c, r.wide);
  return n;
}

void spill(const std::string& dir) {
  tessera::QuerySpec base;
  base.threads = 4;
  size_t ref;
  {
    Parts p = big_parts();
    tessera::QueryResult r;
    tessera::run_query(p, base, r);
    ref = count_cells(r);
  }
  auto attempt = [&](tessera::QuerySpec q, std::string* err, bool* limit) -> size_t {
    Parts p = big_parts();
    tessera::QueryResult r;
    try {
      tessera::run_query(p, q, r);
    } catch (const tessera::SpillError& e) {
      *err = e.what();
      *limit = e.limit;
      return 0;
    } catch (const tessera::OutOfMemory& e) {
      *err = std::string("oom: ") + e.what();
      return 0;
    }
    return count_cells(r);
  };
  std::string err;
  bool limit = false;
  tessera::QuerySpec q = base;
  q.out_budget = 4096;
  q.spill = false;
  attempt(q, &err, &limit);
  report("spill_none", err.rfind("oom: ", 0) == 0, err);
  err.clear();
  q = base;
  q.out_budget = 4096;
  q.spill_dir = dir;
  q.spill_limit = 1 << 16;
  attempt(q, &err, &limit);
  report("spill_limit", !err.empty() && limit, err);
  err.clear();
  limit = false;
  const std::string fresh = dir + "/robust_test_" + std::to_string(getpid());
  q = base;
  q.out_budget = 4096;
  q.spill_dir = fresh;
  attempt(q, &err, &limit);
  report("spill_nodir", !err.empty() && !limit, err);
  err.clear();
  q.spill_mkdir = true;
  const size_t got = attempt(q, &err, &limit);
  report("spill_mkdir", err.empty() && got == ref, err.empty() ? std::to_string(got) + " of " + std::to_string(ref) : err);
  rmdir(fresh.c_str());
}

void threads() {
  size_t ref;
  {
    Parts p = big_parts();
    tessera::QuerySpec q;
    q.threads = 4;
    tessera::QueryResult r;
    tessera::run_query(p, q, r);
    ref = count_cells(r);
  }
  auto one = [&](size_t* out) {
    Parts p = big_parts();
    tessera::QuerySpec q;
    q.threads = 4;
    tessera::QueryResult r;
    tessera::run_query(p, q, r);
    *out = count_cells(r);
  };
  size_t a = 0, b = 0;
  std::thread(one, &a).join();
  std::thread(one, &b).join();
  report("threads_sequential", a == ref && b == ref);
  size_t c[4] = {};
  std::vector<std::thread> ts;
  for (int i = 0; i < 4; i++) ts.emplace_back(one, &c[i]);
  for (auto& t : ts) t.join();
  report("threads_concurrent", c[0] == ref && c[1] == ref && c[2] == ref && c[3] == ref);
  Parts p = big_parts();
  tessera::QuerySpec q;
  q.threads = 1 << 30;
  tessera::QueryResult r;
  tessera::run_query(p, q, r);
  report("threads_clamped", count_cells(r) == ref);
}

void capi() {
  int64_t k[6] = {1, 1, 2, 2, 3, 3}, x[6] = {10, 20, 10, 20, 10, 30};
  char err[256] = {0};
  tessera_query_c* q = tessera_c_new(0);
  tessera_c_add(q, k, x, nullptr, 6);
  int rc = tessera_c_run(q, 0, 1, 0, -1, 2, -1, 0, err, sizeof err);
  const int add_after = tessera_c_add(q, k, x, nullptr, 6);
  const int run_again = tessera_c_run(q, 0, 1, 0, -1, 2, -1, 0, err, sizeof err);
  report("capi_after_run", rc == TESSERA_C_OK && add_after != 0 && run_again == TESSERA_C_ERROR);
  tessera_c_free(q);
  q = tessera_c_new(0);
  tessera_c_add(q, k, x, nullptr, 6);
  tessera_c_set_similarity(q, 9, 0.5);
  rc = tessera_c_run(q, 0, 1, 0, -1, 2, -1, 0, err, sizeof err);
  report("capi_bad_measure", rc == TESSERA_C_ERROR, err);
  tessera_c_free(q);
  q = tessera_c_new(0);
  tessera_dict_c* d = tessera_dict_new();
  tessera_dict_code(d, "a", 1);
  int64_t bad[6] = {0, 0, 5, 0, 0, 0};
  tessera_c_add(q, k, bad, nullptr, 6);
  tessera_c_set_xdict(q, d);
  rc = tessera_c_run(q, 0, 1, 0, -1, 2, -1, 0, err, sizeof err);
  report("capi_bad_code", rc == TESSERA_C_ERROR, err);
  tessera_c_free(q);
  tessera_dict_free(d);
  q = tessera_c_new(0);
  tessera_c_add(q, k, x, nullptr, 6);
  tessera_c_cancel(q);
  rc = tessera_c_run(q, 0, 1, 0, -1, 2, -1, 0, err, sizeof err);
  report("capi_cancel", rc == TESSERA_C_CANCELLED);
  tessera_c_free(q);
}

}

int main(int argc, char** argv) {
  bool full = false;
  std::string dir = "/tmp";
  for (int a = 1; a < argc; a++) {
    if (!strcmp(argv[a], "--full")) full = true;
    else if (!strcmp(argv[a], "--spill-dir") && a + 1 < argc) dir = argv[++a];
    else {
      fprintf(stderr, "usage: robust_test [--full] [--spill-dir d]\n");
      return 2;
    }
  }
  printf("tessera %s, core variant %s\n", tessera_c_version(), tessera_c_isa());
  if (full) topk_ival_exact();
  topk_nonpositive();
  topk_nan();
  having_overflow();
  merge_class("merge_class_small", 0, true);
  merge_class("merge_class_exact", 70000, false);
  part_lengths();
  spill(dir);
  threads();
  capi();
  printf("failures %d\n", fails);
  return fails ? 1 : 0;
}
