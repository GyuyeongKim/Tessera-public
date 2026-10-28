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
  if (oi == oj || h->mode == tessera::PairMode::FULL) {
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
    if (t.i == t.j && h->mode == tessera::PairMode::LT) { h->q++; continue; }
    row_of(h, orig[t.i], orig[t.j], h->cross ? h->res.side[t.i] != 0 : h->second, &x0[n], &x1[n]);
    put(n, out[h->c], h->q);
    n++;
    if (!h->cross && t.i != t.j && h->mode == tessera::PairMode::FULL && !h->second) {
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

void tessera_c_set_xdict(tessera_query_c *h, tessera_dict_c *d) { h->xdict = d; }
void tessera_c_set_ydict(tessera_query_c *h, tessera_dict_c *d) { h->ydict = d; }

static const char *dict_str(const tessera_dict_c *d, int64_t c, size_t *len) {
  if (!d || c < 0 || (size_t)c >= d->d.size()) return nullptr;
  const std::string &s = d->d.str(c);
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
    const std::string &x = d.str((int64_t)c);
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
  try {
    tessera::QuerySpec s;
    s.has_value = h->has_value;
    s.mode = mode == 1 ? tessera::PairMode::LT : mode == 2 ? tessera::PairMode::LE : tessera::PairMode::FULL;
    s.min_value = min_value;
    s.topk = topk > 0 ? topk : -1;
    s.threads = threads > 0 ? threads : omp_get_max_threads();
    s.frontier = frontier;
    s.verbose = verbose != 0;
    s.out_budget = h->out_budget;
    s.spill_dir = h->spill_dir;
    s.spill_limit = h->spill_limit;
    s.should_stop = stop_trampoline;
    s.stop_ctx = h;
    s.release_threads = !h->keep_threads;
    s.similarity = (tessera::Similarity)h->sim_measure;
    s.min_sim = h->min_sim;
    h->mode = s.mode;
    h->col0_smaller = col0_smaller != 0;
    auto recode = [](tessera_dict_c *d, std::vector<std::unique_ptr<tessera::QueryPart>> &parts) {
      const std::vector<int64_t> perm = d->d.finish();
      for (auto &p : parts)
        for (auto &x : p->x) {
          if (x < 0 || (size_t)x >= perm.size())
            throw std::runtime_error("tessera: a group key is not a code of its dictionary");
          x = perm[(size_t)x];
        }
    };
    if (h->xdict) recode(h->xdict, h->parts);
    if (h->ydict) recode(h->ydict, h->parts_y);
    if (h->cross) {
      s.mode = tessera::PairMode::FULL;
      h->mode = s.mode;
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
      if (h->mode == tessera::PairMode::LT) continue;
      row_of(h, oi, oi, false, x0, x1);
      return 1;
    }
    if (h->mode == tessera::PairMode::FULL && !h->second) {
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

size_t tessera_c_next_batch128(tessera_query_c *h, int64_t *x0, int64_t *x1, int64_t *hi, uint64_t *lo, size_t cap) {
  const bool gen = h->res.general, wide = h->res.wide;
  return next_rows(h, x0, x1, cap, [&](size_t n, const tessera::Cells &c, size_t q) {
    __int128 v;
    if (gen) {
      double d;
      memcpy(&d, &c.data[q].s, sizeof d);
      v = (__int128)std::llround(d);
    } else {
      v = tessera::cell_value128(c, q, wide);
    }
    hi[n] = (int64_t)(v >> 64);
    lo[n] = (uint64_t)v;
  });
}

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
