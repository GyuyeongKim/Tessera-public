#include "tessera_c.h"

#include <omp.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <exception>
#include <new>
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
  h->sim_measure = measure >= 1 && measure <= 4 ? measure : 0;
  h->min_sim = min_sim;
}

tessera_query_c *tessera_c_new(int has_value) {
  tessera_query_c *h = new (std::nothrow) tessera_query_c();
  if (!h) return nullptr;
  h->has_value = has_value != 0;
  h->parts.emplace_back(new (std::nothrow) tessera::QueryPart());
  if (!h->parts.back()) { delete h; return nullptr; }
  return h;
}

tessera_query_c *tessera_c_new_cross(int has_value) {
  tessera_query_c *h = tessera_c_new(has_value);
  if (!h) return nullptr;
  h->cross = true;
  h->parts_y.emplace_back(new (std::nothrow) tessera::QueryPart());
  if (!h->parts_y.back()) { delete h; return nullptr; }
  return h;
}

int tessera_c_add_y(tessera_query_c *h, const int64_t *k, const int64_t *y, const double *w, size_t n) {
  if (!h->cross) return -1;
  try {
    tessera::QueryPart &p = *h->parts_y.back();
    p.k.insert(p.k.end(), k, k + n);
    p.x.insert(p.x.end(), y, y + n);
    if (h->has_value) p.v.insert(p.v.end(), w, w + n);
    return 0;
  } catch (...) {
    return -1;
  }
}

int tessera_c_add(tessera_query_c *h, const int64_t *k, const int64_t *x, const double *v, size_t n) {
  try {
    tessera::QueryPart &p = *h->parts.back();
    p.k.insert(p.k.end(), k, k + n);
    p.x.insert(p.x.end(), x, x + n);
    if (h->has_value) p.v.insert(p.v.end(), v, v + n);
    return 0;
  } catch (...) {
    return -1;
  }
}

int tessera_c_run(tessera_query_c *h, int mode, int col0_smaller, uint64_t min_value, int64_t topk, int threads,
                  int64_t frontier, int verbose, char *err, size_t errlen) {
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
    s.should_stop = stop_trampoline;
    s.stop_ctx = h;
    s.release_threads = !h->keep_threads;
    s.similarity = (tessera::Similarity)h->sim_measure;
    s.min_sim = h->min_sim;
    h->mode = s.mode;
    h->col0_smaller = col0_smaller != 0;
    if (h->xdict) {
      const std::vector<int64_t> perm = h->xdict->d.finish();
      for (auto &p : h->parts)
        for (auto &x : p->x) x = perm[(size_t)x];
    }
    if (h->ydict) {
      const std::vector<int64_t> perm = h->ydict->d.finish();
      for (auto &p : h->parts_y)
        for (auto &y : p->x) y = perm[(size_t)y];
    }
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
  } catch (const std::exception &e) {
    set_err(err, errlen, e.what());
    return TESSERA_C_ERROR;
  } catch (...) {
    set_err(err, errlen, "tessera: unknown error");
    return TESSERA_C_ERROR;
  }
}

int tessera_c_next(tessera_query_c *h, int64_t *x0, int64_t *x1, int64_t *vi, double *vd, int *general) {
  const std::vector<tessera::Cells> &out = h->res.out;
  const int64_t *orig = h->res.orig.data();
  while (h->c < out.size()) {
    if (h->q >= out[h->c].n) { h->c++; h->q = 0; continue; }
    const tessera::Triple &t = out[h->c].data[h->q];
    const int64_t oi = orig[t.i], oj = orig[t.j];
    *general = h->res.general ? 1 : 0;
    if (h->res.general) {
      double d;
      memcpy(&d, &t.s, sizeof d);
      *vd = d;
      *vi = (int64_t)std::llround(d);
    } else {
      *vi = (int64_t)t.s;
      *vd = (double)t.s;
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
  const std::vector<tessera::Cells> &out = h->res.out;
  const int64_t *orig = h->res.orig.data();
  const bool gen = h->res.general;
  size_t n = 0;
  while (n < cap && h->c < out.size()) {
    if (h->q >= out[h->c].n) { h->c++; h->q = 0; continue; }
    const tessera::Triple &t = out[h->c].data[h->q];
    if (t.i == t.j && h->mode == tessera::PairMode::LT) { h->q++; continue; }
    double d = (double)t.s;
    if (gen) memcpy(&d, &t.s, sizeof d);
    row_of(h, orig[t.i], orig[t.j], h->cross ? h->res.side[t.i] != 0 : h->second, &x0[n], &x1[n]);
    if (vi) vi[n] = gen ? (int64_t)std::llround(d) : (int64_t)t.s;
    if (vd) vd[n] = d;
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

int tessera_c_general(const tessera_query_c *h) { return h->res.general ? 1 : 0; }

void tessera_c_rewind(tessera_query_c *h) {
  h->c = 0;
  h->q = 0;
  h->second = false;
}

void tessera_c_free(tessera_query_c *h) { delete h; }

}
