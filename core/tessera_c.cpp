#include "tessera_c.h"

#include <omp.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>

#include "query.hpp"

struct tessera_dict_c {
  tessera::StrDict d;
};

struct tessera_query_c {
  bool has_value = false;
  bool cross = false;
  std::vector<std::unique_ptr<tessera::QueryPart>> parts_y;
  tessera_dict_c *xdict = nullptr, *ydict = nullptr;
  size_t out_budget = 0;
  std::string spill_dir;
  uint64_t spill_limit = 0;
  int (*stop_fn)(void *) = nullptr;
  void *stop_ctx = nullptr;
  std::atomic<bool> cancel_req{false};
  bool keep_threads = false;
  int sim_measure = 0;
  double min_sim = 0;
  bool distinct_keys = false;
  size_t many = 0;
  std::vector<uint8_t> many_int;
  bool many_rows = false;
  int many_pair_op = 0;
  int64_t group_topk = -1;
  int group_of = 0;
  bool two = false;
  bool float_errors = false;
  bool float_accum = false;
  double sum_limit = 0;
  int (*pair_pass)(void *, int64_t, int64_t) = nullptr;
  void *pair_ctx = nullptr;
  int pair_bits = 0;
  std::vector<std::unique_ptr<tessera::QueryPart>> parts;
  tessera::QueryResult res;
  tessera::PairMode mode = tessera::PairMode::FULL;
  bool col0_smaller = true;
  bool ran = false;
  size_t c = 0, q = 0;
  bool second = false;
};

namespace {

bool stop_trampoline(void *h) {
  tessera_query_c *q = static_cast<tessera_query_c *>(h);
  return q->cancel_req.load(std::memory_order_relaxed) || (q->stop_fn && q->stop_fn(q->stop_ctx) != 0);
}

void set_err(char *err, size_t errlen, const char *msg) {
  if (err && errlen) {
    strncpy(err, msg, errlen - 1);
    err[errlen - 1] = 0;
  }
}

inline void row_of(const tessera_query_c *h, int64_t oi, int64_t oj, bool second, int64_t *x0, int64_t *x1) {
  if (h->cross) {
    *x0 = second ? oj : oi;
    *x1 = second ? oi : oj;
    return;
  }
  if (oi == oj || tessera::both_orders(h->mode)) {
    *x0 = second ? oj : oi;
    *x1 = second ? oi : oj;
    return;
  }
  const int64_t lo = oi < oj ? oi : oj, hi = oi < oj ? oj : oi;
  *x0 = h->col0_smaller ? lo : hi;
  *x1 = h->col0_smaller ? hi : lo;
}

template <class Put>
size_t next_rows(tessera_query_c *h, int64_t *x0, int64_t *x1, size_t cap, Put put) {
  const std::vector<tessera::Cells> &out = h->res.out;
  const int64_t *orig = h->res.orig.data();
  const bool wide = h->res.wide;
  size_t n = 0;
  while (n < cap && h->c < out.size()) {
    if (h->q >= tessera::cell_count(out[h->c], wide)) { h->c++; h->q = 0; continue; }
    const tessera::Triple &t = out[h->c].data[wide ? 2 * h->q : h->q];
    if (t.i == t.j && !tessera::has_diag(h->mode)) { h->q++; continue; }
    row_of(h, orig[t.i], orig[t.j], h->cross ? h->res.side[t.i] != 0 : h->second, &x0[n], &x1[n]);
    put(n, out[h->c], h->q);
    n++;
    if (!h->cross && t.i != t.j && tessera::both_orders(h->mode) && !h->second) {
      h->second = true;
    } else {
      h->second = false;
      h->q++;
    }
  }
  return n;
}

template <class V>
int add_rows(tessera_query_c *h, tessera::QueryPart *p, const int64_t *k, const int64_t *x, const V *v,
             std::vector<V> tessera::QueryPart::*col, size_t n) {
  if (h->ran || !p) return -1;
  const size_t n0 = p->k.size();
  std::vector<V> &pv = p->*col;
  try {
    p->k.insert(p->k.end(), k, k + n);
    p->x.insert(p->x.end(), x, x + n);
    if (h->has_value) pv.insert(pv.end(), v, v + n);
    return 0;
  } catch (...) {
    p->k.resize(n0);
    p->x.resize(n0);
    if (h->has_value) pv.resize(n0);
    return -1;
  }
}

}

extern "C" {

int tessera_c_api_version(void) { return TESSERA_API_VERSION; }
const char *tessera_c_version(void) { return tessera::kVersion; }
const char *tessera_c_isa(void) { return tessera::isa_name(tessera::active_isa()); }

tessera_dict_c *tessera_dict_new(void) { return new (std::nothrow) tessera_dict_c(); }

int64_t tessera_dict_code(tessera_dict_c *d, const char *s, size_t len) {
  try {
    return d->d.code(s, len);
  } catch (...) {
    return -1;
  }
}

void tessera_dict_free(tessera_dict_c *d) { delete d; }

size_t tessera_dict_size(const tessera_dict_c *d) { return d->d.size(); }

const char *tessera_dict_str(const tessera_dict_c *d, int64_t c, size_t *len) {
  if (c < 0 || (size_t)c >= d->d.size()) return nullptr;
  const tessera::StrDict::Bytes s = d->d.str(c);
  if (len) *len = s.size();
  return s.data();
}

int tessera_dict_set_order(tessera_dict_c *d, const int64_t *rank, size_t n) {
  try {
    return d->d.set_order(std::vector<int64_t>(rank, rank + n)) ? 0 : -1;
  } catch (...) {
    return -1;
  }
}

void tessera_c_set_xdict(tessera_query_c *h, tessera_dict_c *d) { h->xdict = d; }
void tessera_c_set_ydict(tessera_query_c *h, tessera_dict_c *d) { h->ydict = d; }

static const char *dict_str(const tessera_dict_c *d, int64_t c, size_t *len) {
  if (!d || c < 0 || (size_t)c >= d->d.size()) return nullptr;
  const tessera::StrDict::Bytes s = d->d.str(c);
  if (len) *len = s.size();
  return s.data();
}

const char *tessera_c_xstr(const tessera_query_c *h, int64_t x, size_t *len) { return dict_str(h->xdict, x, len); }
const char *tessera_c_ystr(const tessera_query_c *h, int64_t y, size_t *len) { return dict_str(h->ydict, y, len); }

void tessera_c_set_memory(tessera_query_c *h, size_t out_budget, const char *spill_dir) {
  h->out_budget = out_budget;
  h->spill_dir = spill_dir ? spill_dir : "";
}

void tessera_c_set_spill_limit(tessera_query_c *h, uint64_t bytes) { h->spill_limit = bytes; }

void tessera_c_set_stop(tessera_query_c *h, int (*should_stop)(void *), void *ctx) {
  h->stop_fn = should_stop;
  h->stop_ctx = ctx;
}

void tessera_c_cancel(tessera_query_c *h) { h->cancel_req.store(true, std::memory_order_relaxed); }

static size_t dict_strings(const tessera_dict_c *dc, char *bytes, size_t bytes_cap, int64_t *offsets, size_t d_cap,
                           size_t *bytes_out) {
  if (!dc) return 0;
  const tessera::StrDict &d = dc->d;
  size_t total = 0;
  for (size_t c = 0; c < d.size(); c++) total += d.str((int64_t)c).size();
  if (bytes_out) *bytes_out = total;
  if (!bytes || !offsets || bytes_cap < total || d_cap < d.size() + 1) return d.size();
  size_t o = 0;
  for (size_t c = 0; c < d.size(); c++) {
    const tessera::StrDict::Bytes x = d.str((int64_t)c);
    offsets[c] = (int64_t)o;
    memcpy(bytes + o, x.data(), x.size());
    o += x.size();
  }
  offsets[d.size()] = (int64_t)o;
  return d.size();
}

size_t tessera_c_xstrings(const tessera_query_c *h, char *bytes, size_t bytes_cap, int64_t *offsets, size_t d_cap,
                          size_t *bytes_out) {
  return dict_strings(h->xdict, bytes, bytes_cap, offsets, d_cap, bytes_out);
}

size_t tessera_c_ystrings(const tessera_query_c *h, char *bytes, size_t bytes_cap, int64_t *offsets, size_t d_cap,
                          size_t *bytes_out) {
  return dict_strings(h->ydict, bytes, bytes_cap, offsets, d_cap, bytes_out);
}

void tessera_c_set_keep_threads(tessera_query_c *h, int keep) { h->keep_threads = keep != 0; }

void tessera_c_set_similarity(tessera_query_c *h, int measure, double min_sim) {
  h->sim_measure = measure;
  h->min_sim = min_sim;
}

void tessera_c_set_distinct_keys(tessera_query_c *h, int on) { h->distinct_keys = on != 0; }

void tessera_c_set_float_errors(tessera_query_c *h, int on) {
  if (!h->ran) h->float_errors = on != 0;
}

void tessera_c_set_float_accum(tessera_query_c *h, int on) {
  if (!h->ran) h->float_accum = on != 0;
}

int tessera_c_float_errors(const tessera_query_c *h) { return h->res.float_errors; }

void tessera_c_set_sum_limit(tessera_query_c *h, double limit) {
  if (!h->ran) h->sum_limit = limit;
}

void tessera_c_set_many(tessera_query_c *h, size_t n_sums, int pair_op) {
  if (h->ran) return;
  h->many = n_sums;
  h->many_pair_op = pair_op;
}

void tessera_c_set_many_int(tessera_query_c *h, size_t sum, int on) {
  if (h->ran || h->many_rows || sum >= h->many) return;
  h->many_int.resize(h->many, 0);
  h->many_int[sum] = on != 0;
}

int tessera_c_add_many2(tessera_query_c *h, int second_input, const int64_t *k, const int64_t *x, const double *values,
                        const int64_t *ivalues, const uint64_t *nulls, size_t n) {
  if (h->ran || !h->cross || h->many == 0) return -1;
  auto &parts = second_input ? h->parts_y : h->parts;
  if (parts.empty()) return -1;
  const size_t m = h->many;
  auto is_int = [&](size_t s) { return s < h->many_int.size() && h->many_int[s]; };
  for (size_t s = 0; s < m && n > 0; s++) {
    if (is_int(s) ? !ivalues : !values) return -1;
  }
  tessera::QueryPart *p = parts.back().get();
  const size_t n0 = p->k.size();
  try {
    p->sum_v.resize(m);
    p->sum_iv.resize(m);
    p->k.insert(p->k.end(), k, k + n);
    p->x.insert(p->x.end(), x, x + n);
    for (size_t s = 0; s < m; s++) {
      if (is_int(s)) {
        auto &d = p->sum_iv[s];
        d.reserve(d.size() + n);
        for (size_t r = 0; r < n; r++) {
          d.push_back(ivalues[r * m + s]);
        }
      } else {
        auto &d = p->sum_v[s];
        d.reserve(d.size() + n);
        for (size_t r = 0; r < n; r++) {
          d.push_back(values[r * m + s]);
        }
      }
    }
    if (nulls || !p->sum_null.empty()) {
      p->sum_null.resize(n0, 0);
      if (nulls) {
        p->sum_null.insert(p->sum_null.end(), nulls, nulls + n);
      } else {
        p->sum_null.resize(n0 + n, 0);
      }
    }
    h->many_rows = h->many_rows || n > 0;
    return 0;
  } catch (...) {
    p->k.resize(n0);
    p->x.resize(n0);
    for (auto &d : p->sum_v) {
      if (d.size() > n0) d.resize(n0);
    }
    for (auto &d : p->sum_iv) {
      if (d.size() > n0) d.resize(n0);
    }
    if (p->sum_null.size() > n0) p->sum_null.resize(n0);
    return -1;
  }
}

int tessera_c_add_many(tessera_query_c *h, int second_input, const int64_t *k, const int64_t *x, const double *values,
                       size_t n) {
  for (auto i : h->many_int) {
    if (i) return -1;
  }
  return tessera_c_add_many2(h, second_input, k, x, values, nullptr, nullptr, n);
}

size_t tessera_c_next_many2(tessera_query_c *h, int64_t *x0, int64_t *x1, int64_t *count, double *sums, int64_t *isums,
                            uint64_t *nulls, size_t cap) {
  if (h->many == 0 || h->res.stride < 2) return 0;
  const size_t stride = h->res.stride, m = h->many;
  const int64_t *orig = h->res.orig.data();
  const std::vector<tessera::Cells> &out = h->res.out;
  size_t n = 0;
  while (n < cap && h->c < out.size()) {
    if (h->q >= out[h->c].n / stride) {
      h->c++;
      h->q = 0;
      continue;
    }
    const tessera::Triple *t = out[h->c].data.get() + h->q * stride;
    x0[n] = orig[t->i];
    x1[n] = orig[t->j];
    count[n] = (int64_t)t->s;
    for (size_t s = 0; s < m; s++) {
      const auto &sr = h->res.sums[s];
      if (sr.int_values) {
        const uint64_t lo = t[sr.at].s, hi = t[sr.at + 1].s;
        const __int128 v = (__int128)(((unsigned __int128)hi << 64) | lo);
        sums[n * m + s] = (double)v;
        if (isums) {
          isums[2 * (n * m + s)] = (int64_t)lo;
          isums[2 * (n * m + s) + 1] = (int64_t)hi;
        }
      } else {
        memcpy(&sums[n * m + s], &t[sr.at].s, sizeof(double));
        if (isums) {
          isums[2 * (n * m + s)] = isums[2 * (n * m + s) + 1] = 0;
        }
      }
    }
    if (nulls) {
      nulls[n] = h->res.null_at ? t[h->res.null_at].s : 0;
    }
    n++;
    h->q++;
  }
  return n;
}

size_t tessera_c_next_many(tessera_query_c *h, int64_t *x0, int64_t *x1, int64_t *count, double *sums, size_t cap) {
  return tessera_c_next_many2(h, x0, x1, count, sums, nullptr, nullptr, cap);
}

int tessera_c_many_overflow(const tessera_query_c *h, size_t sum, int product_bits, int sum_bits) {
  if (sum >= h->res.sums.size()) return 0;
  return tessera::overflow_flags(h->res.sums[sum], product_bits, sum_bits);
}

void tessera_c_set_group_top(tessera_query_c *h, int64_t k, int of) {
  h->group_topk = k > 0 ? k : -1;
  h->group_of = of;
}

void tessera_c_set_pair_filter(tessera_query_c *h, int (*pass)(void *ctx, int64_t a, int64_t b), void *ctx,
                                int product_bits) {
  if (h->ran) return;
  h->pair_pass = pass;
  h->pair_ctx = ctx;
  h->pair_bits = product_bits;
}

void tessera_c_set_two(tessera_query_c *h, int on) {
  if (!h->ran) h->two = on != 0;
}

tessera_query_c *tessera_c_new(int has_value) {
  tessera_query_c *h = new (std::nothrow) tessera_query_c();
  if (!h) return nullptr;
  h->has_value = has_value != 0;
  try {
    h->parts.emplace_back(new tessera::QueryPart());
  } catch (...) {
    delete h;
    return nullptr;
  }
  return h;
}

tessera_query_c *tessera_c_new_cross(int has_value) {
  tessera_query_c *h = tessera_c_new(has_value);
  if (!h) return nullptr;
  h->cross = true;
  try {
    h->parts_y.emplace_back(new tessera::QueryPart());
  } catch (...) {
    delete h;
    return nullptr;
  }
  return h;
}

int tessera_c_add_y(tessera_query_c *h, const int64_t *k, const int64_t *y, const double *w, size_t n) {
  if (!h->cross || h->parts_y.empty()) return -1;
  return add_rows(h, h->parts_y.back().get(), k, y, w, &tessera::QueryPart::v, n);
}

int tessera_c_add_y_i64(tessera_query_c *h, const int64_t *k, const int64_t *y, const int64_t *w, size_t n) {
  if (!h->cross || h->parts_y.empty()) return -1;
  return add_rows(h, h->parts_y.back().get(), k, y, w, &tessera::QueryPart::iv, n);
}

int tessera_c_add_i64(tessera_query_c *h, const int64_t *k, const int64_t *x, const int64_t *v, size_t n) {
  if (h->parts.empty()) return -1;
  return add_rows(h, h->parts.back().get(), k, x, v, &tessera::QueryPart::iv, n);
}

int tessera_c_add(tessera_query_c *h, const int64_t *k, const int64_t *x, const double *v, size_t n) {
  if (h->parts.empty()) return -1;
  return add_rows(h, h->parts.back().get(), k, x, v, &tessera::QueryPart::v, n);
}

int tessera_c_run(tessera_query_c *h, int mode, int col0_smaller, uint64_t min_value, int64_t topk, int threads,
                  int64_t frontier, int verbose, char *err, size_t errlen) {
  if (h->ran) {
    set_err(err, errlen, "tessera: the query already ran (a handle runs once)");
    return TESSERA_C_ERROR;
  }
  h->ran = true;
  if (h->sim_measure < 0 || h->sim_measure > 4) {
    set_err(err, errlen, "tessera: unknown similarity measure (1 cosine, 2 Jaccard, 3 Dice, 4 overlap)");
    return TESSERA_C_ERROR;
  }
  if (mode < 0 || mode > 3) {
    set_err(err, errlen, "tessera: unknown pair mode (0 all ordered pairs, 1 x0 < x1, 2 x0 <= x1, 3 x0 <> x1)");
    return TESSERA_C_ERROR;
  }
  try {
    tessera::QuerySpec s;
    s.has_value = h->has_value;
    s.mode = (tessera::PairMode)mode;
    s.min_value = min_value;
    s.topk = topk > 0 ? topk : -1;
    s.threads = threads > 0 ? threads : omp_get_max_threads();
    s.frontier = frontier;
    s.verbose = verbose != 0;
    if (h->pair_pass && h->pair_bits >= 2 && h->pair_bits <= 64) {
      s.pair_pass = [](void *c, int64_t a, int64_t b) {
        tessera_query_c *q = static_cast<tessera_query_c *>(c);
        return q->pair_pass(q->pair_ctx, a, b) != 0;
      };
      s.pair_ctx = h;
      s.prod_hi = ((__int128)1 << (h->pair_bits - 1)) - 1;
      s.prod_lo = -s.prod_hi - 1;
    }
    s.float_errors = h->float_errors;
    s.float_accum = h->float_accum;
    s.sum_limit = h->sum_limit;
    s.out_budget = h->out_budget;
    s.spill_dir = h->spill_dir;
    s.spill_limit = h->spill_limit;
    s.should_stop = stop_trampoline;
    s.stop_ctx = h;
    s.release_threads = !h->keep_threads;
    s.similarity = (tessera::Similarity)h->sim_measure;
    s.min_sim = h->min_sim;
    s.distinct_keys = h->distinct_keys;
    if (h->group_of >= 0 && h->group_of <= 4) {
      s.group_topk = h->group_topk;
      s.group_of = (tessera::GroupOf)h->group_of;
    }
    h->mode = s.mode;
    h->col0_smaller = col0_smaller != 0;
    auto recode = [](tessera_dict_c *d, std::vector<std::unique_ptr<tessera::QueryPart>> &parts, bool order) {
      const size_t n = d->d.size();
      for (auto &p : parts)
        for (int64_t x : p->x)
          if (x < 0 || (size_t)x >= n) throw std::runtime_error("tessera: a group key is not a code of its dictionary");
      if (!order) return;
      const std::vector<int64_t> perm = d->d.finish();
      for (auto &p : parts)
        for (auto &x : p->x) x = perm[(size_t)x];
    };
    if (h->xdict) recode(h->xdict, h->parts, !h->cross && !tessera::both_orders(s.mode));
    if (h->ydict) recode(h->ydict, h->parts_y, false);
    if (h->two && !h->has_value) throw std::runtime_error("tessera: two aggregates need a handle with values");
    if (h->cross) {
      s.mode = tessera::PairMode::FULL;
      h->mode = s.mode;
    }
    if (h->many > 0) {
      if (!h->cross || h->has_value) throw std::runtime_error("tessera: several sums need a cross product without values");
      s.has_value = false;
      s.min_value = 0;
      s.topk = -1;
      s.group_topk = -1;
      s.many_pair_op = h->many_pair_op;
      for (auto *ps : {&h->parts, &h->parts_y}) {
        for (auto &p : *ps) {
          p->sum_v.resize(h->many);
          p->sum_iv.resize(h->many);
        }
      }
      tessera::run_query_many(h->parts, h->parts_y, h->many, s, h->res);
    } else if (h->two) {
      tessera::run_query_two(h->parts, h->cross ? &h->parts_y : nullptr, s, h->res);
    } else if (h->cross) {
      tessera::run_query_cross(h->parts, h->parts_y, s, h->res);
    } else {
      tessera::run_query(h->parts, s, h->res);
    }
    tessera_c_rewind(h);
    return TESSERA_C_OK;
  } catch (const tessera::Cancelled &e) {
    set_err(err, errlen, e.what());
    h->res = tessera::QueryResult();
    return TESSERA_C_CANCELLED;
  } catch (const tessera::OutOfMemory &e) {
    set_err(err, errlen, e.what());
    h->res = tessera::QueryResult();
    return TESSERA_C_OUT_OF_MEMORY;
  } catch (const std::bad_alloc &) {
    set_err(err, errlen, "tessera: out of memory");
    h->res = tessera::QueryResult();
    return TESSERA_C_OUT_OF_MEMORY;
  } catch (const tessera::SpillError &e) {
    set_err(err, errlen, e.what());
    h->res = tessera::QueryResult();
    return e.limit ? TESSERA_C_SPILL_LIMIT : TESSERA_C_SPILL_ERROR;
  } catch (const std::exception &e) {
    set_err(err, errlen, e.what());
    h->res = tessera::QueryResult();
    return TESSERA_C_ERROR;
  } catch (...) {
    set_err(err, errlen, "tessera: unknown error");
    h->res = tessera::QueryResult();
    return TESSERA_C_ERROR;
  }
}

int tessera_c_next(tessera_query_c *h, int64_t *x0, int64_t *x1, int64_t *vi, double *vd, int *general) {
  const std::vector<tessera::Cells> &out = h->res.out;
  const int64_t *orig = h->res.orig.data();
  const bool wide = h->res.wide;
  while (h->c < out.size()) {
    if (h->q >= tessera::cell_count(out[h->c], wide)) { h->c++; h->q = 0; continue; }
    const tessera::Triple &t = out[h->c].data[wide ? 2 * h->q : h->q];
    const int64_t oi = orig[t.i], oj = orig[t.j];
    *general = h->res.general ? 1 : 0;
    if (h->res.general) {
      double d;
      memcpy(&d, &t.s, sizeof d);
      *vd = d;
      *vi = (int64_t)std::llround(d);
    } else {
      *vi = (int64_t)t.s;
      *vd = (double)tessera::cell_value128(out[h->c], h->q, wide);
    }
    if (h->cross) {
      h->q++;
      row_of(h, oi, oj, h->res.side[t.i] != 0, x0, x1);
      return 1;
    }
    if (t.i == t.j) {
      h->q++;
      if (!tessera::has_diag(h->mode)) continue;
      row_of(h, oi, oi, false, x0, x1);
      return 1;
    }
    if (tessera::both_orders(h->mode) && !h->second) {
      h->second = true;
      row_of(h, oi, oj, false, x0, x1);
      return 1;
    }
    row_of(h, oi, oj, h->second, x0, x1);
    h->second = false;
    h->q++;
    return 1;
  }
  return 0;
}

size_t tessera_c_next_batch(tessera_query_c *h, int64_t *x0, int64_t *x1, int64_t *vi, double *vd, size_t cap) {
  const bool gen = h->res.general, wide = h->res.wide;
  return next_rows(h, x0, x1, cap, [&](size_t n, const tessera::Cells &c, size_t q) {
    const uint64_t s = c.data[wide ? 2 * q : q].s;
    double d;
    if (gen) memcpy(&d, &s, sizeof d);
    else d = (double)tessera::cell_value128(c, q, wide);
    if (vi) vi[n] = gen ? (int64_t)std::llround(d) : (int64_t)s;
    if (vd) vd[n] = d;
  });
}

static __int128 round128(double d) {
  if (std::isnan(d)) return 0;
  if (d >= 0x1p127) return (__int128)(((unsigned __int128)1 << 127) - 1);
  if (d < -0x1p127) return -(__int128)(((unsigned __int128)1 << 127) - 1) - 1;
  return (__int128)std::nearbyint(d);
}

size_t tessera_c_next_batch128(tessera_query_c *h, int64_t *x0, int64_t *x1, int64_t *hi, uint64_t *lo, size_t cap) {
  const bool gen = h->res.general, wide = h->res.wide;
  return next_rows(h, x0, x1, cap, [&](size_t n, const tessera::Cells &c, size_t q) {
    __int128 v;
    if (gen) {
      double d;
      memcpy(&d, &c.data[q].s, sizeof d);
      v = round128(d);
    } else {
      v = tessera::cell_value128(c, q, wide);
    }
    hi[n] = (int64_t)(v >> 64);
    lo[n] = (uint64_t)v;
  });
}

size_t tessera_c_next_batch2(tessera_query_c *h, int64_t *x0, int64_t *x1, int64_t *count, int64_t *hi, uint64_t *lo,
                             double *vd, size_t cap) {
  if (!h->two || h->res.out2.size() != h->res.out.size()) return 0;
  const bool wide = h->res.wide, gen = h->res.general, gen2 = h->res.general2;
  return next_rows(h, x0, x1, cap, [&](size_t n, const tessera::Cells &c, size_t q) {
    if (gen) {
      double cd;
      memcpy(&cd, &c.data[q].s, sizeof cd);
      count[n] = (int64_t)std::llround(cd);
    } else {
      count[n] = (int64_t)tessera::cell_value128(c, q, wide);
    }
    const __int128 v = h->res.out2[h->c][q];
    double d;
    if (gen2) {
      const uint64_t bits = (uint64_t)v;
      memcpy(&d, &bits, sizeof d);
    } else {
      d = (double)v;
    }
    const __int128 iv = gen2 ? round128(d) : v;
    if (hi) hi[n] = (int64_t)(iv >> 64);
    if (lo) lo[n] = (uint64_t)iv;
    if (vd) vd[n] = d;
  });
}

int tessera_c_general2(const tessera_query_c *h) { return h->res.general2 ? 1 : 0; }

int tessera_c_general(const tessera_query_c *h) { return h->res.general ? 1 : 0; }
int tessera_c_wide(const tessera_query_c *h) { return h->res.wide ? 1 : 0; }
int tessera_c_overflow(const tessera_query_c *h, int product_bits, int sum_bits) {
  return tessera::overflow_flags(h->res, product_bits, sum_bits);
}

void tessera_c_rewind(tessera_query_c *h) {
  h->c = 0;
  h->q = 0;
  h->second = false;
}

void tessera_c_free(tessera_query_c *h) { delete h; }

}
