#include "tessera.hpp"

#include <immintrin.h>
#include <omp.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <parallel/algorithm>
#include <stdexcept>
#include <string>
#ifdef TESSERA_MKL
#include <cmath>
#include <mkl.h>
#endif

#ifndef TESSERA_V
#define TESSERA_V amx
#endif
#ifndef TESSERA_ISA_AMX
#define TESSERA_ISA_AMX 1
#endif
#ifndef TESSERA_GRID_VNNI
#define TESSERA_GRID_VNNI 0
#endif
#define TESSERA_GRID (TESSERA_ISA_AMX || TESSERA_GRID_VNNI)

namespace tessera {
namespace TESSERA_V {

namespace {

constexpr int S = 32;
constexpr int KC = 64;
constexpr int PANEL = S * KC;
constexpr uint64_t POSK = 1000003;
#ifndef PAIR_VARIANT
#define PAIR_VARIANT 0
#endif
#ifndef TILE_LOAD_A
#define TILE_LOAD_A GT_LOAD
#endif
#ifndef TILE_LOAD_B
#define TILE_LOAD_B GT_LOAD
#endif

double now_s() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

#if TESSERA_ISA_AMX
struct alignas(64) TileCfg {
  uint8_t palette_id, start_row, reserved[14];
  uint16_t colsb[16];
  uint8_t rows[16];
};

void amx_init_thread() {
  TileCfg cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.palette_id = 1;
  for (int t = 0; t < 4; t++) { cfg.rows[t] = 16; cfg.colsb[t] = 64; }
  for (int t = 4; t < 6; t++) { cfg.rows[t] = 16; cfg.colsb[t] = 64; }
  for (int t = 6; t < 8; t++) { cfg.rows[t] = 16; cfg.colsb[t] = 64; }
  _tile_loadconfig(&cfg);
}

#define GT_LOAD(t, p, stride) _tile_loadd(t, p, stride)
#define GT_DP(c, a, b) _tile_dpbuud(c, a, b)
#define GT_ZERO(t) _tile_zero(t)
#define GT_STORE(t, p, stride) _tile_stored(t, p, stride)
#define GT_RELEASE() _tile_release()
#elif TESSERA_GRID_VNNI
struct VnniTiles {
  alignas(64) int32_t c[4][256];
  const uint8_t* p[8];
  int stride[8];
};
thread_local VnniTiles vt;

void amx_init_thread() {}

inline void vt_load(int t, const void* p, int stride) {
  if (t < 4) {
    for (int m = 0; m < 16; m++)
      _mm512_store_si512(vt.c[t] + 16 * m, _mm512_loadu_si512((const uint8_t*)p + (size_t)m * stride));
  } else {
    vt.p[t] = (const uint8_t*)p;
    vt.stride[t] = stride;
  }
}

inline void vt_store(int t, void* p, int stride) {
  for (int m = 0; m < 16; m++)
    _mm512_storeu_si512((uint8_t*)p + (size_t)m * stride, _mm512_load_si512(vt.c[t] + 16 * m));
}

inline void vt_zero(int t) {
  for (int m = 0; m < 16; m++) _mm512_store_si512(vt.c[t] + 16 * m, _mm512_setzero_si512());
}

inline void vt_dp(int c, int a, int b) {
  const uint8_t* A = vt.p[a];
  const uint8_t* B = vt.p[b];
  const int sa = vt.stride[a], sb = vt.stride[b];
  int32_t* C = vt.c[c];
  __m512i acc[16];
  for (int m = 0; m < 16; m++) acc[m] = _mm512_load_si512(C + 16 * m);
  for (int k = 0; k < 16; k++) {
    const __m512i bk = _mm512_loadu_si512(B + (size_t)k * sb);
    for (int m = 0; m < 16; m++) {
      int32_t a4;
      memcpy(&a4, A + (size_t)m * sa + 4 * k, 4);
      acc[m] = _mm512_dpbusd_epi32(acc[m], bk, _mm512_set1_epi32(a4));
    }
  }
  for (int m = 0; m < 16; m++) _mm512_store_si512(C + 16 * m, acc[m]);
}

#define GT_LOAD(t, p, stride) vt_load(t, p, stride)
#define GT_DP(c, a, b) vt_dp(c, a, b)
#define GT_ZERO(t) vt_zero(t)
#define GT_STORE(t, p, stride) vt_store(t, p, stride)
#define GT_RELEASE() ((void)0)
#endif

bool amx_permission() {
  static int ok = -1;
  if (ok < 0) ok = syscall(SYS_arch_prctl, 0x1023 , 18 ) == 0;
  return ok;
}

uint8_t* big_zeroed(size_t bytes, int threads) {
  size_t al = 2u << 20, sz = (bytes + al - 1) / al * al;
  if (sz == 0) sz = al;
  uint8_t* p = (uint8_t*)aligned_alloc(al, sz);
  if (!p) throw std::runtime_error("tessera: out of memory (" + std::to_string(sz) + " bytes)");
  madvise(p, sz, MADV_HUGEPAGE);
  size_t step = 1u << 22;
#pragma omp parallel for num_threads(threads) schedule(static)
  for (size_t o = 0; o < sz; o += step) memset(p + o, 0, std::min(step, sz - o));
  return p;
}

void radix_sort_pairs(std::vector<std::pair<uint64_t, int32_t>>& a, int T) {
  constexpr int BITS = 11, NBK = 1 << BITS;
  const size_t n = a.size();
  std::vector<std::pair<uint64_t, int32_t>> b(n);
  std::vector<size_t> hist((size_t)T * NBK);
  for (int shift = 0; shift < 64; shift += BITS) {
#pragma omp parallel num_threads(T)
    {
      const int t = omp_get_thread_num(), nt = omp_get_num_threads();
      const size_t lo = n * t / nt, hi = n * (t + 1) / nt;
      size_t* h = hist.data() + (size_t)t * NBK;
      std::fill(h, h + NBK, 0);
      for (size_t i = lo; i < hi; i++) h[(a[i].first >> shift) & (NBK - 1)]++;
#pragma omp barrier
#pragma omp single
      {
        size_t run = 0;
        for (int d = 0; d < NBK; d++)
          for (int u = 0; u < nt; u++) {
            const size_t c = hist[(size_t)u * NBK + d];
            hist[(size_t)u * NBK + d] = run;
            run += c;
          }
      }
      for (size_t i = lo; i < hi; i++) b[h[(a[i].first >> shift) & (NBK - 1)]++] = a[i];
    }
    a.swap(b);
  }
}

inline uint32_t mh(uint32_t x, uint64_t mult) { return (uint32_t)(((uint64_t)x * mult + 0x632BE59BD9B4E019ull) >> 32); }

inline void add_pair(Checksum& c, uint64_t i, uint64_t j, uint64_t s) {
  c.cells += 2;
  c.sum += 2 * (unsigned __int128)s;
  c.pos += (unsigned __int128)s * (i * POSK + j + j * POSK + i);
}
inline void add_diag(Checksum& c, uint64_t i, uint64_t s) {
  c.cells += 1;
  c.sum += s;
  c.pos += (unsigned __int128)s * (i * POSK + i);
}

constexpr size_t OUT_CHUNK = 1u << 20;

inline double CellD(uint64_t s) {
  double d;
  memcpy(&d, &s, sizeof d);
  return d;
}

struct OutCursor {
  Triple* cur = nullptr;
  Triple* end = nullptr;
  OutSpace* space = nullptr;
  std::vector<Cells> chunks;
  ChunkPtr spare;
  char pad[128 - 3 * sizeof(void*) - sizeof(std::vector<Cells>) - sizeof(ChunkPtr)];
  void close() {
    if (chunks.empty() || !cur) return;
    chunks.back().n = cur - chunks.back().data.get();
    space->settle(chunks.back(), spare);
    cur = end = nullptr;
  }
  __attribute__((noinline)) void open() {
    close();
    chunks.emplace_back();
    chunks.back().data = spare ? std::move(spare) : space->alloc(OUT_CHUNK);
    cur = chunks.back().data.get();
    end = cur + OUT_CHUNK;
  }
};

static_assert(sizeof(OutCursor) == 128, "two cache lines per cursor");

#if TESSERA_GRID
inline void qtrans8(const __m512i* z, __m512i* r) {
  __m512i u[8], v[8];
  for (int a = 0; a < 8; a += 2) {
    u[a] = _mm512_unpacklo_epi64(z[a], z[a + 1]);
    u[a + 1] = _mm512_unpackhi_epi64(z[a], z[a + 1]);
  }
  const __m512i L = _mm512_set_epi64(13, 12, 5, 4, 9, 8, 1, 0), Hh = _mm512_set_epi64(15, 14, 7, 6, 11, 10, 3, 2);
  for (int a = 0; a < 8; a += 4)
    for (int h = 0; h < 2; h++) {
      v[a + h] = _mm512_permutex2var_epi64(u[a + h], L, u[a + 2 + h]);
      v[a + 2 + h] = _mm512_permutex2var_epi64(u[a + h], Hh, u[a + 2 + h]);
    }
  const __m512i L2 = _mm512_set_epi64(11, 10, 9, 8, 3, 2, 1, 0), H2 = _mm512_set_epi64(15, 14, 13, 12, 7, 6, 5, 4);
  for (int k = 0; k < 4; k++) {
    r[k] = _mm512_permutex2var_epi64(v[k], L2, v[k + 4]);
    r[k + 4] = _mm512_permutex2var_epi64(v[k], H2, v[k + 4]);
  }
}

inline void transpose64(const uint64_t* q, uint64_t* t) {
  alignas(64) static const uint8_t i1[64] = {
      56, 48, 40, 32, 24, 16, 8, 0, 57, 49, 41, 33, 25, 17, 9, 1, 58, 50, 42, 34, 26, 18, 10, 2, 59, 51, 43, 35, 27, 19, 11, 3,
      60, 52, 44, 36, 28, 20, 12, 4, 61, 53, 45, 37, 29, 21, 13, 5, 62, 54, 46, 38, 30, 22, 14, 6, 63, 55, 47, 39, 31, 23, 15, 7};
  alignas(64) static const uint8_t i2[64] = {
      0, 8, 16, 24, 32, 40, 48, 56, 1, 9, 17, 25, 33, 41, 49, 57, 2, 10, 18, 26, 34, 42, 50, 58, 3, 11, 19, 27, 35, 43, 51, 59,
      4, 12, 20, 28, 36, 44, 52, 60, 5, 13, 21, 29, 37, 45, 53, 61, 6, 14, 22, 30, 38, 46, 54, 62, 7, 15, 23, 31, 39, 47, 55, 63};
  const __m512i I1 = _mm512_load_si512(i1), I2 = _mm512_load_si512(i2);
  const __m512i ID = _mm512_set1_epi64(0x8040201008040201ll);
  __m512i z[8];
  for (int a = 0; a < 8; a++)
    z[a] = _mm512_gf2p8affine_epi64_epi8(ID, _mm512_permutexvar_epi8(I1, _mm512_loadu_si512(q + 8 * a)), 0);
  __m512i r[8];
  qtrans8(z, r);
  for (int b = 0; b < 8; b++) _mm512_store_si512(t + 8 * b, _mm512_permutexvar_epi8(I2, r[b]));
}
#endif

#if defined(TESSERA_MKL) && TESSERA_ISA_AMX
template <class Emit>
void dense_square(const Csr& X, int H, const Options& opt, Stats& S_, std::vector<Checksum>& part, Emit& emit) {
  const int T = opt.threads;
  const bool has_val = !X.val.empty();
  double t1 = now_s();
  std::vector<int32_t> rows;
  for (int32_t r = 0; r < X.n_rows; r++)
    if (X.ptr[r + 1] > X.ptr[r] && X.idx[X.ptr[r]] < H) rows.push_back(r);
  const int64_t K = rows.size();
  S_.grid_rows = K;
  const bool f32 = opt.dense_mode == DenseMode::SQUARE_F32;
  const size_t esz = f32 ? 4 : 1;
  if (!f32 && has_val && *std::max_element(X.val.begin(), X.val.end()) > 127) {
    fprintf(stderr, "tessera: the s8u8 GEMM needs values <= 127\n");
    exit(2);
  }
  uint8_t* D = big_zeroed((size_t)K * H * esz, T);
#pragma omp parallel for num_threads(T) schedule(static)
  for (int64_t p = 0; p < K; p++) {
    int32_t r = rows[p];
    for (int64_t e = X.ptr[r]; e < X.ptr[r + 1] && X.idx[e] < H; e++) {
      uint8_t v = has_val ? X.val[e] : 1;
      if (f32) ((float*)D)[(size_t)p * H + X.idx[e]] = v;
      else D[(size_t)p * H + X.idx[e]] = v;
    }
  }
  int32_t* C = (int32_t*)big_zeroed((size_t)H * H * 4, T);
  double t2 = now_s();
  S_.t_panels = t2 - t1;
  mkl_set_num_threads(T);
  if (f32) {
    cblas_ssyrk(CblasRowMajor, CblasUpper, CblasTrans, H, K, 1.0f, (const float*)D, H, 0.0f, (float*)C, H);
  } else {
    const int HB = std::min(H, 1024);
    const MKL_INT32 co = 0;
    for (int bi = 0; bi < H; bi += HB)
      for (int bj = bi; bj < H; bj += HB) {
        const int m = std::min(HB, H - bi), n = std::min(HB, H - bj);
        cblas_gemm_s8u8s32(CblasRowMajor, CblasTrans, CblasNoTrans, CblasFixOffset, m, n, K, 1.0f, D + bi, H, 0,
                           D + bj, H, 0, 0.0f, C + (size_t)bi * H + bj, H, &co);
      }
  }
  double t3 = now_s();
#pragma omp parallel num_threads(T)
  {
    int tid = omp_get_thread_num();
    Checksum& ck = part[tid];
#pragma omp for schedule(dynamic, 16)
    for (int i = 0; i < H; i++)
      for (int j = i; j < H; j++) {
        uint64_t s = f32 ? (uint64_t)llroundf(((const float*)C)[(size_t)i * H + j]) : (uint64_t)(uint32_t)C[(size_t)i * H + j];
        if (s) emit(tid, ck, i, j, s);
      }
  }
  free(D);
  free(C);
  S_.t_dense = now_s() - t2;
  S_.tdpbuud = 0;
  (void)t3;
}
#endif

}

struct Stopper {
  bool (*fn)(void*);
  void* ctx;
  std::atomic<bool> flag;
  Stopper(bool (*f)(void*), void* c) : fn(f), ctx(c), flag(false) {}
  bool operator()(int tid) {
    if (fn && tid == 0 && !flag.load(std::memory_order_relaxed) && fn(ctx)) flag.store(true, std::memory_order_relaxed);
    return flag.load(std::memory_order_relaxed);
  }
};

std::string u128(unsigned __int128 v) {
  if (v == 0) return "0";
  char b[64];
  int n = 0;
  while (v) { b[n++] = '0' + (int)(v % 10); v /= 10; }
  std::string s(b, n);
  std::reverse(s.begin(), s.end());
  return s;
}

Checksum xtx(const Csr& X, const Options& opt, Stats* st) {
  Stats local;
  Stats& S_ = st ? *st : local;
  S_ = Stats();
  const int T = opt.threads;
  const bool general = !X.wval.empty();
  const bool cross = opt.cross_hx >= 0;
  const int HC = cross ? opt.H : 0, CHX = cross ? opt.cross_hx : 0, CLX = cross ? opt.cross_lx_end : 0;
  const bool grid_vals = !TESSERA_GRID_VNNI || X.val.empty() || *std::max_element(X.val.begin(), X.val.end()) <= 127;
  const int H = !(TESSERA_GRID && opt.dense && !general && X.ival.empty() && grid_vals) ? 0
                : cross                                     ? HC
                                                            : (std::min(opt.H, X.n_cols) + S - 1) / S * S;
  const int co_order = cross ? 0 : opt.co_order;
  (void)co_order;
  const bool has_val = !X.val.empty();
  const bool wide = !X.ival.empty();
  std::vector<Checksum> part(T);
  const bool ck_on = opt.checksum;
  std::vector<OutCursor> outc(opt.out ? T : 0);
  OutSpace space;
  space.budget = opt.out_budget;
  space.dir = opt.spill_dir;
  for (auto& oc : outc) oc.space = &space;
  const bool out_on = opt.out != nullptr;
  Stopper stop(opt.should_stop, opt.stop_ctx);
  const uint64_t min_value = opt.min_value;
  const int sim = opt.sim_measure;
  auto emit = [&](int tid, Checksum& ck, int64_t i, int64_t j, uint64_t s) {
    if (sim) {
      if (i == j && !opt.sim_diag) return;
      const double v = similarity(sim, general ? CellD(s) : (double)s, opt.sim_n[i], opt.sim_n[j]);
      if (v < opt.min_sim) return;
      memcpy(&s, &v, sizeof s);
    }
    if (min_value && (general ? CellD(s) < (double)min_value : s < min_value)) return;
    if (ck_on) { if (i == j) add_diag(ck, i, s); else add_pair(ck, i, j, s); }
    if (out_on) {
      OutCursor& oc = outc[tid];
      if (oc.cur == oc.end) oc.open();
      *oc.cur++ = {(int32_t)i, (int32_t)j, s};
    }
  };
  double t0 = now_s();

  const int H1 = (opt.pair_frontier > 0 && opt.pair_frontier < H && !general && !cross && opt.dense_mode == DenseMode::GRID)
                     ? opt.pair_frontier / S * S
                     : H;
  const bool pairs_on = H1 < H;
  std::vector<int32_t> fpre, fbeg;
#if defined(TESSERA_MKL) && TESSERA_ISA_AMX
  if (H > 0 && opt.dense_mode != DenseMode::GRID && !cross) {
    dense_square(X, H, opt, S_, part, emit);
  } else
#endif
#if TESSERA_GRID
  if (H > 0) {
#if TESSERA_ISA_AMX
    if (!amx_permission()) throw std::runtime_error("tessera: AMX permission denied");
#endif
    const int NB = H / S;
    std::vector<int32_t> rows;
    std::vector<uint64_t> key;
#ifdef TESSERA_HL_PROFILE
    double po[4] = {now_s(), 0, 0, 0};
#endif
    {
      std::vector<uint64_t> k(X.n_rows, ~0ull);
#pragma omp parallel for num_threads(T) schedule(static)
      for (int32_t r = 0; r < X.n_rows; r++) {
        uint32_t m1 = ~0u, m2 = ~0u;
        bool any = false, side1 = false;
        const int KH = (X.ptr[r + 1] > X.ptr[r] && X.idx[X.ptr[r]] < H1) ? H1 : H;
        for (int64_t e = X.ptr[r]; e < X.ptr[r + 1] && X.idx[e] < KH; e++) {
          any = true;
          side1 = side1 || X.idx[e] >= CHX;
          m1 = std::min(m1, mh(X.idx[e], 0x9E3779B97F4A7C15ull));
          m2 = std::min(m2, mh(X.idx[e], 0xD6E8FEB86659FD93ull));
        }
        if (cross) any = any && side1 && X.idx[X.ptr[r]] < CHX;
        if (any) {
          if (!opt.reorder) k[r] = (uint64_t)r;
          else if (!pairs_on) k[r] = (uint64_t)m1 << 32 | m2;
          else k[r] = (uint64_t)(KH == H1 ? 0 : 1) << 63 | (uint64_t)(m1 >> 1) << 32 | m2;
        }
      }
#ifdef TESSERA_HL_PROFILE
      po[1] = now_s();
#endif
      std::vector<std::pair<uint64_t, int32_t>> kr;
      kr.reserve(X.n_rows);
      for (int32_t r = 0; r < X.n_rows; r++)
        if (k[r] != ~0ull) kr.push_back({k[r], r});
#ifdef TESSERA_HL_PROFILE
      po[2] = now_s();
#endif
      radix_sort_pairs(kr, T);
#ifdef TESSERA_HL_PROFILE
      po[3] = now_s();
      fprintf(stderr, "# order profile (s): t0->keys %.4f keys %.4f pairs %.4f sort %.4f\n", po[0] - t0, po[1] - po[0],
              po[2] - po[1], po[3] - po[2]);
#endif
      rows.resize(kr.size());
      for (size_t p = 0; p < kr.size(); p++) rows[p] = kr[p].second;
    }
    std::vector<int32_t> pi(H), ipi(H);
    for (int32_t i = 0; i < H; i++) pi[i] = ipi[i] = i;
    for (int round = 0; opt.co_mode == 2 && round < co_order; round++) {
      std::vector<std::pair<uint64_t, int32_t>> ki(H);
      {
        std::vector<std::vector<uint32_t>> m1(T, std::vector<uint32_t>(H, ~0u)), m2(T, std::vector<uint32_t>(H, ~0u));
#pragma omp parallel for num_threads(T) schedule(static)
        for (size_t p = 0; p < rows.size(); p++) {
          const int32_t r = rows[p];
          const uint32_t c = (uint32_t)(p / KC);
          const uint32_t h1 = mh(c, 0x9E3779B97F4A7C15ull), h2 = mh(c, 0xD6E8FEB86659FD93ull);
          auto& a = m1[omp_get_thread_num()];
          auto& b = m2[omp_get_thread_num()];
          for (int64_t e = X.ptr[r]; e < X.ptr[r + 1] && X.idx[e] < H; e++) {
            a[X.idx[e]] = std::min(a[X.idx[e]], h1);
            b[X.idx[e]] = std::min(b[X.idx[e]], h2);
          }
        }
        for (int32_t i = 0; i < H; i++) {
          uint32_t a = ~0u, b = ~0u;
          for (int t = 0; t < T; t++) { a = std::min(a, m1[t][i]); b = std::min(b, m2[t][i]); }
          ki[i] = {(uint64_t)a << 32 | b, i};
        }
      }
      std::sort(ki.begin(), ki.end());
      for (int32_t q = 0; q < H; q++) { pi[ki[q].second] = q; ipi[q] = ki[q].second; }
      if (round + 1 == co_order) break;
      std::vector<std::pair<uint64_t, int32_t>> kr(rows.size());
#pragma omp parallel for num_threads(T) schedule(static)
      for (size_t p = 0; p < rows.size(); p++) {
        const int32_t r = rows[p];
        uint32_t a = ~0u, b = ~0u;
        for (int64_t e = X.ptr[r]; e < X.ptr[r + 1] && X.idx[e] < H; e++) {
          const uint32_t tile = (uint32_t)(pi[X.idx[e]] >> 4);
          a = std::min(a, mh(tile, 0x9E3779B97F4A7C15ull));
          b = std::min(b, mh(tile, 0xD6E8FEB86659FD93ull));
        }
        kr[p] = {(uint64_t)a << 32 | b, r};
      }
      __gnu_parallel::sort(kr.begin(), kr.end());
      for (size_t p = 0; p < kr.size(); p++) rows[p] = kr[p].second;
    }
    for (int round = 0; opt.co_mode != 2 && round < co_order; round++) {
      if (round > 0) {
        std::vector<std::pair<double, int32_t>> kr(rows.size());
#pragma omp parallel for num_threads(T) schedule(static)
        for (size_t p = 0; p < rows.size(); p++) {
          const int32_t r = rows[p];
          double sum = 0, cnt = 0;
          for (int64_t e = X.ptr[r]; e < X.ptr[r + 1] && X.idx[e] < H; e++) { sum += pi[X.idx[e]]; cnt++; }
          kr[p] = {sum / cnt, r};
        }
        __gnu_parallel::stable_sort(kr.begin(), kr.end());
        for (size_t p = 0; p < kr.size(); p++) rows[p] = kr[p].second;
      }
      std::vector<std::vector<double>> sum(T, std::vector<double>(H, 0.0)), cnt(T, std::vector<double>(H, 0.0));
#pragma omp parallel for num_threads(T) schedule(static)
      for (size_t p = 0; p < rows.size(); p++) {
        const int32_t r = rows[p];
        auto& su = sum[omp_get_thread_num()];
        auto& cn = cnt[omp_get_thread_num()];
        for (int64_t e = X.ptr[r]; e < X.ptr[r + 1] && X.idx[e] < H; e++) { su[X.idx[e]] += (double)p; cn[X.idx[e]] += 1; }
      }
      std::vector<std::pair<double, int32_t>> ki(H);
      for (int32_t i = 0; i < H; i++) {
        double su = 0, cn = 0;
        for (int t = 0; t < T; t++) { su += sum[t][i]; cn += cnt[t][i]; }
        ki[i] = {cn > 0 ? su / cn : 1e300, i};
      }
      std::stable_sort(ki.begin(), ki.end());
      for (int32_t q = 0; q < H; q++) { pi[ki[q].second] = q; ipi[q] = ki[q].second; }
    }
    const int64_t nrows = rows.size();
    const int64_t nchunks = (nrows + KC - 1) / KC;
    const int64_t nwords = (nchunks + 63) / 64;
    S_.grid_rows = nrows;
    S_.chunks = nchunks;
    S_.blocks = NB;
    double t1 = now_s();
    S_.t_order = t1 - t0;

#ifdef TESSERA_HL_PROFILE
    double pp_[5] = {now_s(), 0, 0, 0, 0};
#endif
    std::vector<uint64_t> bm((size_t)NB * nwords, 0);
#pragma omp parallel num_threads(T)
    {
      std::vector<uint64_t> loc(NB);
#pragma omp for schedule(dynamic, 1)
      for (int64_t w = 0; w < nwords; w++) {
        std::fill(loc.begin(), loc.end(), 0);
        for (int64_t c = w * 64; c < std::min(nchunks, (w + 1) * 64); c++) {
          uint64_t bit = 1ull << (c & 63);
          for (int64_t p = c * KC; p < std::min(nrows, (c + 1) * KC); p++) {
            int32_t r = rows[p];
            for (int64_t e = X.ptr[r]; e < X.ptr[r + 1] && X.idx[e] < H; e++) loc[pi[X.idx[e]] / S] |= bit;
          }
        }
        for (int b = 0; b < NB; b++)
          if (loc[b]) bm[(size_t)b * nwords + w] = loc[b];
      }
    }
#ifdef TESSERA_HL_PROFILE
    pp_[1] = now_s();
#endif
    std::vector<uint32_t> pre((size_t)NB * nwords);
    std::vector<int64_t> off(NB + 1, 0);
#pragma omp parallel for num_threads(T) schedule(static)
    for (int b = 0; b < NB; b++) {
      uint32_t acc = 0;
      for (int64_t w = 0; w < nwords; w++) { pre[(size_t)b * nwords + w] = acc; acc += __builtin_popcountll(bm[(size_t)b * nwords + w]); }
      off[b + 1] = acc;
    }
    for (int b = 0; b < NB; b++) off[b + 1] += off[b];
    const int64_t npanels = off[NB];
    S_.panels = npanels;
#ifdef TESSERA_HL_PROFILE
    pp_[2] = now_s();
#endif
    uint8_t* A = big_zeroed((size_t)npanels * PANEL, T);
    uint8_t* B = big_zeroed((size_t)npanels * PANEL, T);
    uint8_t* HM = big_zeroed((size_t)npanels, T);
    std::vector<uint16_t> pcnt(pairs_on ? npanels : 0);
#ifdef TESSERA_HL_PROFILE
    pp_[3] = now_s();
#endif
#pragma omp parallel for num_threads(T) schedule(dynamic, 1)
    for (int64_t w = 0; w < nwords; w++) {
      for (int64_t c = w * 64; c < std::min(nchunks, (w + 1) * 64); c++) {
        uint64_t below = (1ull << (c & 63)) - 1;
        for (int64_t p = c * KC; p < std::min(nrows, (c + 1) * KC); p++) {
          int32_t r = rows[p];
          int k = (int)(p - c * KC);
          for (int64_t e = X.ptr[r]; e < X.ptr[r + 1] && X.idx[e] < H; e++) {
            int it = pi[X.idx[e]], b = it / S, m = it % S;
            size_t bw = (size_t)b * nwords + w;
            int64_t px = off[b] + pre[bw] + __builtin_popcountll(bm[bw] & below);
            uint8_t v = has_val ? X.val[e] : 1;
            A[(size_t)px * PANEL + m * KC + k] = v;
            B[(size_t)px * PANEL + (k >> 2) * (S * 4) + m * 4 + (k & 3)] = v;
            HM[px] |= (uint8_t)(1 << (m >> 4));
            if (pairs_on) pcnt[px]++;
          }
        }
      }
    }
#ifdef TESSERA_HL_PROFILE
    pp_[4] = now_s();
    fprintf(stderr, "# panel profile (s): bitmaps %.4f ranks %.4f zeroed %.4f (%.2f GB) fill %.4f\n", pp_[1] - pp_[0],
            pp_[2] - pp_[1], pp_[3] - pp_[2], 2.0 * npanels * PANEL / 1e9, pp_[4] - pp_[3]);
#endif
    const int NB1 = H1 / S;
    fpre.assign(NB, 0);
    for (int b = 0; b < NB1; b++) fpre[b] = b + 1;
    if (cross)
      for (int b = 0; b < NB; b++) fpre[b] = b < CHX / S ? 0 : CHX / S;
    fbeg.assign(NB, 0);
    if (opt.sim_lo)
      for (int b = 0; b < NB; b++) fbeg[b] = std::min(b, opt.sim_lo[(size_t)b * S] / S);
    if (pairs_on) {
#pragma omp parallel for num_threads(T) schedule(dynamic, 1)
      for (int b2 = NB1; b2 < NB; b2++) {
        int b1 = 0;
        for (; b1 <= b2; b1++) {
          double td = 0, est = 0;
          for (int64_t w = 0; w < nwords; w++) {
            const uint64_t m1 = bm[(size_t)b1 * nwords + w], m2 = bm[(size_t)b2 * nwords + w];
            uint64_t x = m1 & m2;
            while (x) {
              const uint64_t low = (x & -x) - 1;
              const int64_t i1 = off[b1] + pre[(size_t)b1 * nwords + w] + __builtin_popcountll(m1 & low);
              const int64_t i2 = off[b2] + pre[(size_t)b2 * nwords + w] + __builtin_popcountll(m2 & low);
              td += __builtin_popcount(HM[i1]) * __builtin_popcount(HM[i2]);
              est += (double)pcnt[i1] * pcnt[i2] / KC;
              x &= x - 1;
            }
          }
          if (opt.pair_tdp_ns * td > opt.pair_prod_ns * est) break;
        }
        fpre[b2] = b1;
      }
      for (int b = NB1; b < NB; b++) S_.pair_prefix += fpre[b];
    }
    double t2 = now_s();
    S_.t_panels = t2 - t1;

    std::vector<uint64_t> tdp(T, 0), npair(T, 0);
    const bool permuted = co_order > 0;
    auto emit_block = [&](int tid, Checksum& ck, int b1, int b2, const int32_t* cb) {
      const uint64_t i0 = (uint64_t)b1 * S, j0 = (uint64_t)b2 * S;
      for (int m = 0; m < S; m++)
        for (int q = (b1 == b2 ? m : 0); q < S; q++) {
          int32_t s = cb[m * S + q];
          if (!s) continue;
          if (permuted) emit(tid, ck, ipi[i0 + m], ipi[j0 + q], (uint32_t)s);
          else emit(tid, ck, i0 + m, j0 + q, (uint32_t)s);
        }
    };
#define TESSERA_TILE_PRODUCTS(pa, pb, h1, h2)                                                         \
    if (((h1) & (h2)) == 3) {                                                                        \
      TILE_LOAD_A(4, pa, KC);                                                                        \
      TILE_LOAD_B(6, pb, S * 4);                                                                     \
      TILE_LOAD_A(5, (pa) + 16 * KC, KC);                                                            \
      TILE_LOAD_B(7, (pb) + 64, S * 4);                                                              \
      GT_DP(0, 4, 6);                                                                         \
      GT_DP(1, 4, 7);                                                                         \
      GT_DP(2, 5, 6);                                                                         \
      GT_DP(3, 5, 7);                                                                         \
      n += 4;                                                                                        \
    } else {                                                                                         \
      if ((h1) & 1) TILE_LOAD_A(4, pa, KC);                                                          \
      if ((h1) & 2) TILE_LOAD_A(5, (pa) + 16 * KC, KC);                                              \
      if ((h2) & 1) TILE_LOAD_B(6, pb, S * 4);                                                       \
      if ((h2) & 2) TILE_LOAD_B(7, (pb) + 64, S * 4);                                                \
      if ((h1) & (h2) & 1) GT_DP(0, 4, 6);                                                    \
      if (((h1) & 1) && ((h2) & 2)) GT_DP(1, 4, 7);                                           \
      if (((h1) & 2) && ((h2) & 1)) GT_DP(2, 5, 6);                                           \
      if ((h1) & (h2) & 2) GT_DP(3, 5, 7);                                                    \
      n += __builtin_popcount(h1) * __builtin_popcount(h2);                                          \
    }
#if PAIR_VARIANT == 2
#define TESSERA_PAIR_WORD(x, m1w, m2w, base1, base2)                                                 \
    {                                                                                                \
      const uint8_t* pas_[64];                                                                       \
      const uint8_t* pbs_[64];                                                                       \
      uint8_t hs_[64];                                                                               \
      int nc_ = 0;                                                                                   \
      while (x) {                                                                                    \
        const uint64_t low = (x & -x) - 1;                                                           \
        const int64_t i1 = (base1) + __builtin_popcountll((m1w) & low);                              \
        const int64_t i2 = (base2) + __builtin_popcountll((m2w) & low);                              \
        pas_[nc_] = A + (size_t)i1 * PANEL;                                                          \
        pbs_[nc_] = B + (size_t)i2 * PANEL;                                                          \
        hs_[nc_++] = (uint8_t)(HM[i1] | HM[i2] << 2);                                                \
        x &= x - 1;                                                                                  \
      }                                                                                              \
      for (int q_ = 0; q_ < nc_; q_++) {                                                             \
        const unsigned h1_ = hs_[q_] & 3, h2_ = hs_[q_] >> 2;                                        \
        TESSERA_TILE_PRODUCTS(pas_[q_], pbs_[q_], h1_, h2_)                                          \
      }                                                                                              \
    }
#elif PAIR_VARIANT == 1
#define TESSERA_PAIR_WORD(x, m1w, m2w, base1, base2)                                                 \
    while (x) {                                                                                      \
      uint64_t low = (x & -x) - 1;                                                                   \
      const int64_t i1 = (base1) + __builtin_popcountll((m1w) & low);                                \
      const int64_t i2 = (base2) + __builtin_popcountll((m2w) & low);                                \
      const uint8_t* pa = A + (size_t)i1 * PANEL;                                                    \
      const uint8_t* pb = B + (size_t)i2 * PANEL;                                                    \
      const unsigned h1 = HM[i1], h2 = HM[i2];                                                       \
      TESSERA_TILE_PRODUCTS(pa, pb, h1, h2)                                                          \
      x &= x - 1;                                                                                    \
    }
#else
#define TESSERA_PAIR_WORD(x, m1w, m2w, base1, base2)                                                 \
    while (x) {                                                                                      \
      uint64_t low = (x & -x) - 1;                                                                   \
      const int64_t i1 = (base1) + __builtin_popcountll((m1w) & low);                                \
      const int64_t i2 = (base2) + __builtin_popcountll((m2w) & low);                                \
      const uint8_t* pa = A + (size_t)i1 * PANEL;                                                    \
      const uint8_t* pb = B + (size_t)i2 * PANEL;                                                    \
      const unsigned h1 = HM[i1], h2 = HM[i2];                                                       \
      if (h1 & 1) TILE_LOAD_A(4, pa, KC);                                                            \
      if (h1 & 2) TILE_LOAD_A(5, pa + 16 * KC, KC);                                                  \
      if (h2 & 1) TILE_LOAD_B(6, pb, S * 4);                                                         \
      if (h2 & 2) TILE_LOAD_B(7, pb + 64, S * 4);                                                    \
      if (h1 & h2 & 1) GT_DP(0, 4, 6);                                                        \
      if ((h1 & 1) && (h2 & 2)) GT_DP(1, 4, 7);                                               \
      if ((h1 & 2) && (h2 & 1)) GT_DP(2, 5, 6);                                               \
      if (h1 & h2 & 2) GT_DP(3, 5, 7);                                                        \
      n += __builtin_popcount(h1) * __builtin_popcount(h2);                                          \
      x &= x - 1;                                                                                    \
    }
#endif
    if (opt.group <= 0) {
#pragma omp parallel num_threads(T)
      {
        int tid = omp_get_thread_num();
        amx_init_thread();
        alignas(64) int32_t cb[S * S];
        Checksum& ck = part[tid];
#pragma omp for schedule(dynamic, 1)
        for (int b1 = 0; b1 < NB; b1++) {
          if (stop(tid)) continue;
          const uint64_t* m1 = &bm[(size_t)b1 * nwords];
          const uint32_t* p1 = &pre[(size_t)b1 * nwords];
          for (int b2 = b1; b2 < NB; b2++) {
            if (b1 >= fpre[b2] || b1 < fbeg[b2]) continue;
            const uint64_t* m2 = &bm[(size_t)b2 * nwords];
            const uint32_t* p2 = &pre[(size_t)b2 * nwords];
            uint64_t n = 0;
            GT_ZERO(0); GT_ZERO(1); GT_ZERO(2); GT_ZERO(3);
            for (int64_t w = 0; w < nwords; w++) {
              uint64_t x = m1[w] & m2[w];
              TESSERA_PAIR_WORD(x, m1[w], m2[w], off[b1] + p1[w], off[b2] + p2[w])
            }
            if (!n) continue;
            npair[tid]++;
            tdp[tid] += n;
            GT_STORE(0, cb, S * 4);
            GT_STORE(1, cb + 16, S * 4);
            GT_STORE(2, cb + 16 * S, S * 4);
            GT_STORE(3, cb + 16 * S + 16, S * 4);
            emit_block(tid, ck, b1, b2, cb);
          }
        }
        GT_RELEASE();
      }
    } else {
      const int G = opt.group;
      const int64_t RB = opt.range_panels > 0 ? opt.range_panels : 64 * (int64_t)G;
      const size_t CT = (size_t)S * S;
      int32_t* cbuf = (int32_t*)big_zeroed((size_t)NB * G * CT * sizeof(int32_t), T);
      std::vector<uint8_t> touched((size_t)NB * G);
      std::vector<int64_t> rcut;
      std::vector<uint64_t> pw(T, 0);
      for (int J0 = 0; J0 < NB; J0 += G) {
        const int J1 = std::min(NB, J0 + G);
        const int U = std::max(1, std::min(opt.unit, J1 / T));
        const int nunits = (J1 + U - 1) / U;
        std::fill(touched.begin(), touched.begin() + (size_t)J1 * G, 0);
        rcut.assign(1, 0);
        for (int64_t w = 0, acc = 0; w < nwords; w++) {
          int64_t c = 0;
          for (int b2 = J0; b2 < J1; b2++) c += __builtin_popcountll(bm[(size_t)b2 * nwords + w]);
          if (c > RB) {
            if (acc > 0) rcut.push_back(w * 64);
            int parts = 2;
            while (parts < 64 && c / parts > RB) parts *= 2;
            for (int q = 1; q < parts; q++) rcut.push_back(w * 64 + q * (64 / parts));
            rcut.push_back((w + 1) * 64);
            acc = 0;
            continue;
          }
          if (acc > 0 && acc + c > RB) { rcut.push_back(w * 64); acc = 0; }
          acc += c;
        }
        if (rcut.back() != nwords * 64) rcut.push_back(nwords * 64);
        S_.ranges += rcut.size() - 1;
#pragma omp parallel num_threads(T)
        {
          int tid = omp_get_thread_num();
          amx_init_thread();
          uint64_t n = 0;
          for (size_t r = 0; r + 1 < rcut.size(); r++) {
            const int64_t ca = rcut[r], cb_ = rcut[r + 1];
            const int64_t wa = ca >> 6, wb = (cb_ + 63) >> 6;
            const uint64_t mlo = ~0ull << (ca & 63);
            const uint64_t mhi = (cb_ & 63) ? (1ull << (cb_ & 63)) - 1 : ~0ull;
#pragma omp for schedule(dynamic, 1)
            for (int u = 0; u < nunits; u++) {
              if (stop(tid)) continue;
              const int a0 = u * U, a1 = std::min(J1, a0 + U);
              for (int b2 = std::max(J0, a0); b2 < J1; b2++) {
                const uint64_t* m2 = &bm[(size_t)b2 * nwords];
                const uint32_t* p2 = &pre[(size_t)b2 * nwords];
                const int b1end = std::min(std::min(a1, b2 + 1), fpre[b2]);
                for (int b1 = std::max(a0, fbeg[b2]); b1 < b1end; b1++) {
                  const uint64_t* m1 = &bm[(size_t)b1 * nwords];
                  const uint32_t* p1 = &pre[(size_t)b1 * nwords];
                  const size_t ci = (size_t)b1 * G + (b2 - J0);
                  int32_t* cb = cbuf + ci * CT;
                  bool live = false;
                  for (int64_t w = wa; w < wb; w++) {
                    uint64_t x = m1[w] & m2[w];
                    if (w == wa) x &= mlo;
                    if (w == wb - 1) x &= mhi;
                    if (!x) continue;
                    if (!live) {
                      live = true;
                      if (touched[ci]) {
                        GT_LOAD(0, cb, S * 4);
                        GT_LOAD(1, cb + 16, S * 4);
                        GT_LOAD(2, cb + 16 * S, S * 4);
                        GT_LOAD(3, cb + 16 * S + 16, S * 4);
                      } else {
                        GT_ZERO(0); GT_ZERO(1); GT_ZERO(2); GT_ZERO(3);
                        touched[ci] = 1;
                      }
                    }
                    TESSERA_PAIR_WORD(x, m1[w], m2[w], off[b1] + p1[w], off[b2] + p2[w])
                  }
                  if (!live) continue;
                  pw[tid]++;
                  GT_STORE(0, cb, S * 4);
                  GT_STORE(1, cb + 16, S * 4);
                  GT_STORE(2, cb + 16 * S, S * 4);
                  GT_STORE(3, cb + 16 * S + 16, S * 4);
                }
              }
            }
          }
          tdp[tid] += n;
          GT_RELEASE();
          Checksum& ck = part[tid];
#pragma omp for schedule(dynamic, 1)
          for (int b1 = 0; b1 < J1; b1++)
            for (int b2 = std::max(J0, b1); b2 < J1; b2++) {
              const size_t ci = (size_t)b1 * G + (b2 - J0);
              if (!touched[ci]) continue;
              npair[tid]++;
              emit_block(tid, ck, b1, b2, cbuf + ci * CT);
            }
        }
      }
      free(cbuf);
      for (int t = 0; t < T; t++) S_.c_reloads += pw[t];
    }
#undef TESSERA_PAIR_WORD
#undef TESSERA_TILE_PRODUCTS
    for (int t = 0; t < T; t++) { S_.tdpbuud += tdp[t]; S_.block_pairs += npair[t]; }
    free(A);
    free(B);
    free(HM);
    S_.t_dense = now_s() - t2;
  }
#endif

  const int32_t M = X.n_cols;
  std::vector<int64_t> cptr;
  std::vector<int32_t> crow;
  std::vector<uint8_t> cval;
  std::vector<uint32_t> cival;
  std::vector<double> cwval;
  const int HS = H1;
  if (HS < M && (opt.sparse || opt.hl)) {
    double tc = now_s();
    cptr.assign(M + 1, 0);
    std::vector<std::vector<int64_t>> cnt(T);
#pragma omp parallel num_threads(T)
    {
      auto& c = cnt[omp_get_thread_num()];
      c.assign(M - HS, 0);
#pragma omp for schedule(static)
      for (int32_t r = 0; r < X.n_rows; r++)
        for (int64_t e = X.ptr[r + 1] - 1; e >= X.ptr[r] && X.idx[e] >= HS; e--) c[X.idx[e] - HS]++;
    }
    for (int32_t j = HS; j < M; j++) {
      int64_t run = cptr[j];
      for (int t = 0; t < T; t++) { int64_t n = cnt[t][j - HS]; cnt[t][j - HS] = run; run += n; }
      cptr[j + 1] = run;
    }
    crow.resize(cptr[M]);
    if (has_val) cval.resize(cptr[M]);
    if (wide) cival.resize(cptr[M]);
    if (general) cwval.resize(cptr[M]);
#pragma omp parallel num_threads(T)
    {
      auto& c = cnt[omp_get_thread_num()];
#pragma omp for schedule(static)
      for (int32_t r = 0; r < X.n_rows; r++)
        for (int64_t e = X.ptr[r + 1] - 1; e >= X.ptr[r] && X.idx[e] >= HS; e--) {
          int64_t o = c[X.idx[e] - HS]++;
          crow[o] = r;
          if (has_val) cval[o] = X.val[e];
          if (wide) cival[o] = X.ival[e];
          if (general) cwval[o] = X.wval[e];
        }
    }
    S_.t_colidx = now_s() - tc;
  }

  const int HH = opt.hl_frontier > 0 ? std::min(opt.hl_frontier, H) : H;
  const bool hl_on =
      TESSERA_GRID && opt.hl && !has_val && !general && !pairs_on && !cross && !sim && HH > 0 && H < M;
#ifdef TESSERA_HL_PROFILE
  std::vector<std::array<uint64_t, 8>> hprof(T);
#define HP_MARK(t) const uint64_t t = __rdtsc()
#define HP_ADD(k, a, b) hprof[tid][k] += (b) - (a)
#else
#define HP_MARK(t)
#define HP_ADD(k, a, b)
#endif
#if TESSERA_GRID
  if (hl_on) {
    double th = now_s();
#if TESSERA_ISA_AMX
    if (!amx_permission()) throw std::runtime_error("tessera: AMX permission denied");
#endif
    const int HW = (HH + 63) / 64;
    uint64_t* HB = (uint64_t*)big_zeroed(((size_t)X.n_rows + 1) * HW * 8, T);
#ifdef TESSERA_HL_PROFILE
    fprintf(stderr, "# hl HB zeroed (s): %.4f (%.2f GB)\n", now_s() - th, ((size_t)X.n_rows + 1) * HW * 8 / 1e9);
#endif
#pragma omp parallel for num_threads(T) schedule(static)
    for (int32_t r = 0; r < X.n_rows; r++)
      for (int64_t e = X.ptr[r]; e < X.ptr[r + 1] && X.idx[e] < HH; e++)
        HB[(size_t)r * HW + (X.idx[e] >> 6)] |= 1ull << (X.idx[e] & 63);
    std::vector<int32_t> lperm(M - H);
    for (int32_t j = H; j < M; j++) lperm[j - H] = j;
    if (opt.hl_order) {
      std::vector<std::pair<uint64_t, int32_t>> kj(M - H);
#pragma omp parallel for num_threads(T) schedule(dynamic, 256)
      for (int32_t j = H; j < M; j++) {
        uint32_t m1 = ~0u, m2 = ~0u;
        for (int64_t o = cptr[j]; o < cptr[j + 1]; o++) {
          m1 = std::min(m1, mh((uint32_t)crow[o], 0x9E3779B97F4A7C15ull));
          m2 = std::min(m2, mh((uint32_t)crow[o], 0xD6E8FEB86659FD93ull));
        }
        kj[j - H] = {(uint64_t)m1 << 32 | m2, j};
      }
      __gnu_parallel::sort(kj.begin(), kj.end());
      for (size_t q = 0; q < kj.size(); q++) lperm[q] = kj[q].second;
    }
    S_.t_hl_build = now_s() - th;
    const int64_t nbatch = (M - H + 15) / 16;
#ifdef TESSERA_HL_PROFILE
    std::vector<double> hbusy(T, 0.0);
#endif
    std::vector<uint64_t> htd(T, 0), hpan(T, 0), hrows(T, 0);
#pragma omp parallel num_threads(T)
    {
      const int tid = omp_get_thread_num();
      amx_init_thread();
      Checksum& ck = part[tid];
      std::vector<int32_t> slot(X.n_rows, -1), R;
      std::vector<uint16_t> memb;
      std::vector<uint8_t, std::allocator<uint8_t>> braw;
      std::vector<uint64_t> qg;
      alignas(64) uint64_t tq[3 * 64];
      alignas(64) uint8_t Ab[3 * 64 * 64];
      alignas(64) int32_t cb[4 * 256];
      const __m512i ones = _mm512_set1_epi8(1);
#pragma omp for schedule(dynamic, 4)
      for (int64_t bi = 0; bi < nbatch; bi++) {
        if (stop(tid)) continue;
        const int32_t q0 = (int32_t)bi * 16, jn = std::min<int32_t>(16, M - H - q0);
        const int32_t* bj = lperm.data() + q0;
        HP_MARK(c0);
        R.clear();
        memb.clear();
        for (int jj = 0; jj < jn; jj++)
          for (int64_t o = cptr[bj[jj]]; o < cptr[bj[jj] + 1]; o++) {
            const int32_t r = crow[o];
            if (X.idx[X.ptr[r]] >= HH) continue;
            if (slot[r] < 0) { slot[r] = R.size(); R.push_back(r); memb.push_back(0); }
            memb[slot[r]] |= (uint16_t)(1u << jj);
          }
        for (int32_t r : R) slot[r] = -1;
        if (R.empty()) continue;
        const int64_t np = (R.size() + 63) / 64;
        while ((int64_t)R.size() < np * 64) { R.push_back(X.n_rows); memb.push_back(0); }
        hpan[tid] += np;
        hrows[tid] += R.size();
        HP_MARK(c1);
        HP_ADD(0, c0, c1);
        braw.resize(np * 1024 + 64);
        uint8_t* Bp = (uint8_t*)(((uintptr_t)braw.data() + 63) & ~(uintptr_t)63);
        for (int64_t k = 0; k < np * 64; k += 4) {
          const uint64_t w = _pdep_u64(memb[k], 0x1111111111111111ull) | _pdep_u64(memb[k + 1], 0x2222222222222222ull) |
                             _pdep_u64(memb[k + 2], 0x4444444444444444ull) | _pdep_u64(memb[k + 3], 0x8888888888888888ull);
          _mm512_store_si512(Bp + k * 16, _mm512_maskz_mov_epi8(w, ones));
        }
        HP_MARK(c2);
        HP_ADD(1, c1, c2);
        auto mul = [&](const uint8_t* A4, int64_t p) {
          GT_LOAD(6, Bp + p * 1024, 64);
          GT_LOAD(4, A4, 64);
          GT_LOAD(5, A4 + 1024, 64);
          GT_DP(0, 4, 6);
          GT_DP(1, 5, 6);
          GT_LOAD(4, A4 + 2048, 64);
          GT_LOAD(5, A4 + 3072, 64);
          GT_DP(2, 4, 6);
          GT_DP(3, 5, 6);
        };
        auto expand = [&](uint64_t* tqb, uint8_t* A4) {
          for (int c = 0; c < 64; c++)
            _mm512_store_si512(A4 + c * 64, _mm512_maskz_mov_epi8(_load_mask64((__mmask64*)(tqb + c)), ones));
        };
        HP_MARK(c3);
        const int64_t K = np * 64;
        qg.resize((size_t)HW * K + 8);
        uint64_t* QG = (uint64_t*)(((uintptr_t)qg.data() + 63) & ~(uintptr_t)63);
        {
          const int HW8 = HW & ~7;
          for (int64_t k = 0; k < K; k += 8) {
            const uint64_t* hb[8];
            for (int a = 0; a < 8; a++) hb[a] = HB + (size_t)R[k + a] * HW;
            for (int g = 0; g < HW8; g += 8) {
              __m512i z[8], r[8];
              for (int a = 0; a < 8; a++) z[a] = _mm512_loadu_si512(hb[a] + g);
              qtrans8(z, r);
              for (int b = 0; b < 8; b++) _mm512_store_si512(QG + (size_t)(g + b) * K + k, r[b]);
            }
            for (int g = HW8; g < HW; g++)
              for (int a = 0; a < 8; a++) QG[(size_t)g * K + k + a] = hb[a][g];
          }
        }
        HP_MARK(c4);
        HP_ADD(6, c3, c4);
        for (int g = 0; g < HW; g++) {
          GT_ZERO(0); GT_ZERO(1); GT_ZERO(2); GT_ZERO(3);
          int64_t pp[3] = {-1, -1, -1};
          int64_t t = 0;
          for (int64_t p = 0; p <= np + 1; p++) {
            HP_MARK(d0);
            const int s1 = t % 3, s2 = (t + 2) % 3, s3 = (t + 1) % 3;
            bool adv = false;
            if (p < np) {
              const uint64_t* qp = QG + (size_t)g * K + p * 64;
              __m512i orv = _mm512_load_si512(qp);
              for (int k = 8; k < 64; k += 8) orv = _mm512_or_si512(orv, _mm512_load_si512(qp + k));
              if (_mm512_test_epi64_mask(orv, orv)) { transpose64(qp, tq + s1 * 64); pp[s1] = p; adv = true; htd[tid] += 4; }
            } else {
              pp[s1] = -1;
              adv = true;
            }
            if (!adv) continue;
            HP_MARK(d1);
            HP_ADD(2, d0, d1);
            if (pp[s2] >= 0) expand(tq + s2 * 64, Ab + s2 * 4096);
            HP_MARK(d2);
            HP_ADD(3, d1, d2);
            if (pp[s3] >= 0) { mul(Ab + s3 * 4096, pp[s3]); pp[s3] = -1; }
            HP_MARK(d3);
            HP_ADD(4, d2, d3);
            t++;
          }
          if (t <= 2) continue;
          HP_MARK(e0);
          GT_STORE(0, cb, 64);
          GT_STORE(1, cb + 256, 64);
          GT_STORE(2, cb + 512, 64);
          GT_STORE(3, cb + 768, 64);
          for (int c = 0; c < 64; c++) {
            const int32_t i = g * 64 + c;
            if (i >= HH) break;
            const __m512i row = _mm512_load_si512(cb + c * 16);
            for (uint32_t m = _mm512_test_epi32_mask(row, row) & ((1u << jn) - 1); m; m &= m - 1) {
              const int jj = __builtin_ctz(m);
              emit(tid, ck, i, bj[jj], (uint32_t)cb[c * 16 + jj]);
            }
          }
          HP_MARK(e1);
          HP_ADD(5, e0, e1);
        }
      }
      GT_RELEASE();
#ifdef TESSERA_HL_PROFILE
      hbusy[tid] = now_s() - th;
#endif
    }
    free(HB);
#ifdef TESSERA_HL_PROFILE
    {
      std::array<uint64_t, 8> tot{};
      for (auto& a : hprof)
        for (int k = 0; k < 8; k++) tot[k] += a[k];
      double mx = 0, mn = 1e30;
      for (double b : hbusy) { mx = std::max(mx, b); mn = std::min(mn, b); }
      fprintf(stderr, "# hl threads finish (s since start): first %.4f last %.4f; build %.4f\n", mn, mx, S_.t_hl_build);
      fprintf(stderr, "# hl profile (Gcycles, all threads): gather %.2f bbuild %.2f pregather %.2f test+transpose %.2f expand %.2f mul %.2f "
              "tail+emit %.2f\n", tot[0] * 1e-9, tot[1] * 1e-9, tot[6] * 1e-9, tot[2] * 1e-9, tot[3] * 1e-9, tot[4] * 1e-9,
              tot[5] * 1e-9);
    }
#endif
#undef HP_MARK
#undef HP_ADD
    for (int t = 0; t < T; t++) { S_.hl_tdpbuud += htd[t]; S_.hl_panels += hpan[t]; S_.hl_rows += hrows[t]; }
    S_.t_hl = now_s() - th;
  }
#endif

  if (opt.sparse && cross && HS < X.n_cols) {
    double t3 = now_s();
    std::vector<int64_t> rb((size_t)X.n_rows * 3);
#pragma omp parallel for num_threads(T) schedule(static)
    for (int32_t r = 0; r < X.n_rows; r++) {
      const auto b = X.idx.begin() + X.ptr[r], e = X.idx.begin() + X.ptr[r + 1];
      rb[3 * (size_t)r] = std::lower_bound(b, e, CHX) - X.idx.begin();
      rb[3 * (size_t)r + 1] = std::lower_bound(b, e, HC) - X.idx.begin();
      rb[3 * (size_t)r + 2] = std::lower_bound(b, e, CLX) - X.idx.begin();
    }
    std::vector<uint64_t> prod(T, 0);
#pragma omp parallel num_threads(T)
    {
      const int tid = omp_get_thread_num();
      std::vector<uint64_t> acc(general ? 0 : M, 0);
      std::vector<double> accd(general ? M : 0, 0.0);
      std::vector<uint32_t> cnt(general ? M : 0, 0);
      std::vector<int32_t> touched;
      touched.reserve(M);
      Checksum& ck = part[tid];
      uint64_t np = 0;
#pragma omp for schedule(dynamic, 16)
      for (int32_t j = std::max(HS, CHX); j < M; j++) {
        if (stop(tid)) continue;
        for (int64_t o = cptr[j]; o < cptr[j + 1]; o++) {
          const int32_t r = crow[o];
          const int64_t* q = &rb[3 * (size_t)r];
          int64_t lo[2], hi[2];
          int nr = 1;
          if (j < HC) { lo[0] = X.ptr[r]; hi[0] = q[0]; }
          else if (j < CLX) { lo[0] = q[0]; hi[0] = q[1]; }
          else { lo[0] = X.ptr[r]; hi[0] = q[0]; lo[1] = q[1]; hi[1] = q[2]; nr = 2; }
          if (general) {
            const double vj = cwval[o];
            for (int s = 0; s < nr; s++)
              for (int64_t e = lo[s]; e < hi[s]; e++) {
                const int32_t i = X.idx[e];
                if (!cnt[i]++) touched.push_back(i);
                accd[i] += X.wval[e] * vj;
                np++;
              }
          } else {
            const uint64_t vj = wide ? cival[o] : has_val ? cval[o] : 1;
            for (int s = 0; s < nr; s++)
              for (int64_t e = lo[s]; e < hi[s]; e++) {
                const int32_t i = X.idx[e];
                if (!acc[i]) touched.push_back(i);
                acc[i] += (wide ? X.ival[e] : has_val ? X.val[e] : 1) * vj;
                np++;
              }
          }
        }
        if (general) {
          for (int32_t i : touched) {
            uint64_t b;
            memcpy(&b, &accd[i], sizeof b);
            emit(tid, ck, i, j, b);
            accd[i] = 0;
            cnt[i] = 0;
          }
        } else {
          for (int32_t i : touched) { emit(tid, ck, i, j, acc[i]); acc[i] = 0; }
        }
        touched.clear();
      }
      prod[tid] = np;
    }
    for (int t = 0; t < T; t++) S_.sparse_products += prod[t];
    S_.t_sparse = now_s() - t3;
  }

  if (opt.sparse && !cross && HS < X.n_cols) {
    double t3 = now_s();
    std::vector<int64_t> lstart;
    if (hl_on) {
      lstart.resize(X.n_rows);
#pragma omp parallel for num_threads(T) schedule(static)
      for (int32_t r = 0; r < X.n_rows; r++)
        lstart[r] = std::lower_bound(X.idx.begin() + X.ptr[r], X.idx.begin() + X.ptr[r + 1], HH) - X.idx.begin();
    }
    std::vector<uint64_t> prod(T, 0);
    if (general) {
#pragma omp parallel num_threads(T)
      {
        int tid = omp_get_thread_num();
        std::vector<double> acc(M, 0.0);
        std::vector<uint32_t> cnt(M, 0);
        std::vector<int32_t> touched;
        touched.reserve(M);
        Checksum& ck = part[tid];
        uint64_t np = 0;
#pragma omp for schedule(dynamic, 16)
        for (int32_t j = 0; j < M; j++) {
          if (stop(tid)) continue;
          double diag = 0;
          for (int64_t o = cptr[j]; o < cptr[j + 1]; o++) {
            const int32_t r = crow[o];
            const double vj = cwval[o];
            diag += vj * vj;
            for (int64_t e = X.ptr[r]; e < X.ptr[r + 1]; e++) {
              const int32_t i = X.idx[e];
              if (i >= j) break;
              if (!cnt[i]++) touched.push_back(i);
              acc[i] += X.wval[e] * vj;
              np++;
            }
          }
          for (int32_t i : touched) {
            uint64_t b;
            memcpy(&b, &acc[i], sizeof b);
            emit(tid, ck, i, j, b);
            acc[i] = 0;
            cnt[i] = 0;
          }
          touched.clear();
          if (cptr[j + 1] > cptr[j]) {
            uint64_t b;
            memcpy(&b, &diag, sizeof b);
            emit(tid, ck, j, j, b);
          }
        }
        prod[tid] = np;
      }
    } else {
#pragma omp parallel num_threads(T)
    {
      int tid = omp_get_thread_num();
      std::vector<uint64_t> acc(M, 0);
      std::vector<int32_t> touched;
      touched.reserve(M);
      Checksum& ck = part[tid];
      uint64_t np = 0;
#pragma omp for schedule(dynamic, 16)
      for (int32_t j = HS; j < M; j++) {
        if (stop(tid)) continue;
        uint64_t diag = 0;
        int32_t istart = j < H ? S * fpre[j / S] : 0;
        if (opt.sim_lo) istart = std::max(istart, opt.sim_lo[j]);
        for (int64_t o = cptr[j]; o < cptr[j + 1]; o++) {
          int32_t r = crow[o];
          uint64_t vj = wide ? cival[o] : has_val ? cval[o] : 1;
          diag += vj * vj;
          int64_t e0 = hl_on ? lstart[r] : X.ptr[r];
          if (istart > 0) e0 = std::lower_bound(X.idx.begin() + X.ptr[r], X.idx.begin() + X.ptr[r + 1], istart) - X.idx.begin();
          for (int64_t e = e0; e < X.ptr[r + 1]; e++) {
            int32_t i = X.idx[e];
            if (i >= j) break;
            uint64_t vi = wide ? X.ival[e] : has_val ? X.val[e] : 1;
            if (!acc[i]) touched.push_back(i);
            acc[i] += vi * vj;
            np++;
          }
        }
        for (int32_t i : touched) { emit(tid, ck, i, j, acc[i]); acc[i] = 0; }
        touched.clear();
        if (diag && (j >= H || fpre[j / S] <= j / S)) emit(tid, ck, j, j, diag);
      }
      prod[tid] = np;
    }
    }
    for (int t = 0; t < T; t++) S_.sparse_products += prod[t];
    S_.t_sparse = now_s() - t3;
  }

  if (opt.out) {
    opt.out->clear();
    for (auto& oc : outc) {
      oc.close();
      space.release(oc.spare);
      for (auto& c : oc.chunks) opt.out->push_back(std::move(c));
    }
  }
  S_.spilled_chunks = space.spilled.load();
  S_.spill_failed = space.spill_failed.load();
  S_.cancelled = stop.flag.load();
  Checksum total;
  for (auto& c : part) total.add(c);
  S_.t_total = now_s() - t0;
  return total;
}

struct RowList {
  std::unique_ptr<int32_t[]> r;
  int64_t n = 0;
  int64_t size() const { return n; }
  int32_t operator[](int64_t i) const { return r[i]; }
};
static RowList flagged_rows(const std::vector<uint8_t>& flag, int T) {
  const int64_t N = (int64_t)flag.size();
  std::vector<int64_t> cnt((size_t)T + 1, 0);
#pragma omp parallel for num_threads(T) schedule(static)
  for (int t = 0; t < T; t++) {
    int64_t c = 0;
    for (int64_t r = N * t / T; r < N * (t + 1) / T; r++) c += flag[r] != 0;
    cnt[t + 1] = c;
  }
  for (int t = 0; t < T; t++) cnt[t + 1] += cnt[t];
  RowList out;
  out.n = cnt[T];
  out.r.reset(new int32_t[std::max<int64_t>(out.n, 1)]);
#pragma omp parallel for num_threads(T) schedule(static)
  for (int t = 0; t < T; t++) {
    int64_t p = cnt[t];
    for (int64_t r = N * t / T; r < N * (t + 1) / T; r++)
      if (flag[r]) out.r[p++] = (int32_t)r;
  }
  return out;
}

int choose_frontier_cross(const Csr& X, const std::vector<uint8_t>& side, int T, const CostModel& cm, std::string* log) {
  const int M = X.n_cols;
  const int HMAX = std::min(M, 1 << 16);
  std::vector<int> cand;
  cand.push_back(0);
  for (int h = 1024; h < HMAX; h *= 2) cand.push_back(h);
  if (HMAX > 0) cand.push_back(HMAX);
  const int NC = cand.size();
  std::vector<int32_t> pos(M);
  std::vector<int64_t> below0(NC), below1(NC);
  {
    int32_t c0 = 0, c1 = 0;
    for (int32_t c = 0; c < M; c++) pos[c] = side[c] ? c1++ : c0++;
    for (int q = 0; q < NC; q++) {
      int64_t a = 0, b = 0;
      for (int32_t c = 0; c < cand[q]; c++) (side[c] ? b : a)++;
      below0[q] = a;
      below1[q] = b;
    }
  }
  const size_t AS = (3 * NC + 1 + 7) / 8 * 8 + 8;
  std::vector<double> accbuf((size_t)T * AS, 0.0);
  auto acc_of = [&](int t) { return accbuf.data() + (size_t)t * AS; };
  std::vector<uint8_t> both(X.n_rows, 0);
#pragma omp parallel for num_threads(T) schedule(dynamic, 4096)
  for (int32_t r = 0; r < X.n_rows; r++) {
    double* a = acc_of(omp_get_thread_num());
    double hx = 0, hy = 0;
    int q = 0;
    for (int64_t e = X.ptr[r]; e < X.ptr[r + 1]; e++) {
      const int32_t c = X.idx[e];
      for (; q < NC && c >= cand[q]; q++) {
        a[q] += hx * hy;
        if (hx > 0 && hy > 0) { a[NC + q] += 1; a[2 * NC + q] += hx + hy; }
      }
      (side[c] ? hy : hx) += 1;
    }
    for (; q < NC; q++) {
      a[q] += hx * hy;
      if (hx > 0 && hy > 0) { a[NC + q] += 1; a[2 * NC + q] += hx + hy; }
    }
    a[3 * NC] += hx * hy;
    bool s0 = false, s1 = false;
    for (int64_t e = X.ptr[r]; e < X.ptr[r + 1] && X.idx[e] < HMAX; e++) (side[X.idx[e]] ? s1 : s0) = true;
    both[r] = s0 && s1;
  }
  std::vector<double> ph(NC, 0.0), gr(NC, 0.0), gz(NC, 0.0);
  double ptot = 0;
  for (int t = 0; t < T; t++) {
    const double* a = acc_of(t);
    for (int q = 0; q < NC; q++) { ph[q] += a[q]; gr[q] += a[NC + q]; gz[q] += a[2 * NC + q]; }
    ptot += a[3 * NC];
  }
  const RowList rows = flagged_rows(both, T);
  const int64_t nchunks = (rows.size() + KC - 1) / KC;
  const int64_t step = std::max<int64_t>(1, nchunks / 4096);
  const int NW = (HMAX + 1023) / 1024;
  std::vector<std::vector<double>> tdl(T, std::vector<double>(NC, 0.0));
#pragma omp parallel num_threads(T)
  {
    std::vector<uint64_t> b0(NW), b1(NW);
    auto& my = tdl[omp_get_thread_num()];
#pragma omp for schedule(dynamic, 16)
    for (int64_t c = 0; c < nchunks; c += step) {
      std::fill(b0.begin(), b0.end(), 0);
      std::fill(b1.begin(), b1.end(), 0);
      for (int64_t p = c * KC; p < std::min<int64_t>(rows.size(), (c + 1) * KC); p++) {
        const int32_t r = rows[p];
        for (int64_t e = X.ptr[r]; e < X.ptr[r + 1] && X.idx[e] < HMAX; e++) {
          const int32_t t = pos[X.idx[e]] >> 4;
          (side[X.idx[e]] ? b1 : b0)[t >> 6] |= 1ull << (t & 63);
        }
      }
      auto active = [&](const std::vector<uint64_t>& b, int64_t items) {
        const int64_t tiles = (items + 15) / 16;
        double a = 0;
        for (int64_t w = 0; w < tiles / 64; w++) a += __builtin_popcountll(b[w]);
        if (tiles % 64) a += __builtin_popcountll(b[tiles / 64] & ((1ull << (tiles % 64)) - 1));
        return a;
      };
      for (int q = 0; q < NC; q++) my[q] += active(b0, below0[q]) * active(b1, below1[q]) * step;
    }
  }
  int best = 0;
  double bestT = 1e300;
  char buf[256];
  for (int q = 0; q < NC; q++) {
    double tdp = 0;
    for (int t = 0; t < T; t++) tdp += tdl[t][q];
    const double nb0 = below0[q] / S, nb1 = below1[q] / S;
    double t = cm.sparse_product * (ptot - ph[q]);
    if (nb0 >= 1 && nb1 >= 1)
      t += cm.tdpbuud * tdp + cm.block_pair * nb0 * nb1 + cm.grid_row * gr[q] + cm.grid_nnz * gz[q];
    else
      t = cm.sparse_product * ptot;
    if (log) {
      snprintf(buf, sizeof buf, "H %d (%lld + %lld) tdpbuud %.3g heavy %.3g of %.3g grid_rows %.3g model %.3f s\n", cand[q],
               (long long)below0[q], (long long)below1[q], tdp, ph[q], ptot, gr[q], t);
      *log += buf;
    }
    if (t < bestT) { bestT = t; best = cand[q]; }
  }
  return best;
}

int choose_frontier(const Csr& X, int T, const CostModel& cm, std::string* log, bool hl) {
  const int HMAX = std::min(X.n_cols, 1 << 16);
  std::vector<int> cand;
  cand.push_back(0);
  for (int h = 1024; h < HMAX; h *= 2) cand.push_back(h);
  if (HMAX > 0) cand.push_back((HMAX + S - 1) / S * S);
  const int NC = cand.size();
  std::vector<double> ph(NC, 0.0), lr(NC, 0.0), pll(NC, 0.0), gr(NC, 0.0), gz(NC, 0.0);
  const size_t AS = (5 * NC + 1 + 7) / 8 * 8 + 8;
  std::vector<double> accbuf((size_t)T * AS, 0.0);
  auto acc_of = [&](int t) { return accbuf.data() + (size_t)t * AS; };
  std::vector<uint8_t> qual(X.n_rows);
#pragma omp parallel for num_threads(T) schedule(dynamic, 4096)
  for (int32_t r = 0; r < X.n_rows; r++) {
    const int32_t* b = X.idx.data() + X.ptr[r];
    const int32_t* e = X.idx.data() + X.ptr[r + 1];
    if (e == b) continue;
    double* a = acc_of(omp_get_thread_num());
    const double n = e - b;
    a[5 * NC] += n * (n + 1) / 2;
    qual[r] = *b < HMAX;
    for (int c = 0; c < NC; c++) {
      double h = std::lower_bound(b, e, cand[c]) - b, l = (e - b) - h;
      a[c] += h * (h + 1) / 2;
      if (h > 0) { a[NC + c] += l; a[3 * NC + c] += 1; a[4 * NC + c] += h; }
      a[2 * NC + c] += l * (l + 1) / 2;
    }
  }
  for (int t = 0; t < T; t++)
    for (int c = 0; c < NC; c++) {
      const double* a = acc_of(t);
      ph[c] += a[c]; lr[c] += a[NC + c]; pll[c] += a[2 * NC + c]; gr[c] += a[3 * NC + c]; gz[c] += a[4 * NC + c];
    }
  double ptot = 0;
  for (int t = 0; t < T; t++) ptot += acc_of(t)[5 * NC];
  const RowList rows = flagged_rows(qual, T);
  const int64_t nchunks = (rows.size() + KC - 1) / KC;
  const int64_t step = std::max<int64_t>(1, nchunks / 4096);
  const int NW = (cand.back() + 1023) / 1024;
  std::vector<double> tdp(NC, 0.0);
  std::vector<std::vector<double>> tdl(T, std::vector<double>(NC, 0.0));
#pragma omp parallel num_threads(T)
  {
    std::vector<uint64_t> bits(NW);
    auto& my = tdl[omp_get_thread_num()];
#pragma omp for schedule(dynamic, 16)
    for (int64_t c = 0; c < nchunks; c += step) {
      std::fill(bits.begin(), bits.end(), 0);
      for (int64_t p = c * KC; p < std::min<int64_t>(rows.size(), (c + 1) * KC); p++) {
        int32_t r = rows[p];
        for (int64_t e = X.ptr[r]; e < X.ptr[r + 1] && X.idx[e] < HMAX; e++) bits[X.idx[e] >> 10] |= 1ull << ((X.idx[e] >> 4) & 63);
      }
      for (int q = 0; q < NC; q++) {
        int tiles = cand[q] / 16;
        double a = 0;
        for (int w = 0; w < tiles / 64; w++) a += __builtin_popcountll(bits[w]);
        if (tiles % 64) a += __builtin_popcountll(bits[tiles / 64] & ((1ull << (tiles % 64)) - 1));
        my[q] += a * (a + 1) / 2 * step;
      }
    }
  }
  for (int t = 0; t < T; t++)
    for (int q = 0; q < NC; q++) tdp[q] += tdl[t][q];
  int best = 0;
  double bestT = 1e300;
  char buf[256];
  for (int q = 0; q < NC; q++) {
    const double nb = cand[q] / S;
    double t = cm.tdpbuud * tdp[q] + cm.block_pair * nb * (nb + 1) / 2;
    if (cand[q] > 0) t += cm.grid_row * gr[q] + cm.grid_nnz * gz[q];
    if (hl && cand[q] > 0)
      t += cm.hl_row_group * lr[q] * ((cand[q] + 63) / 64) + cm.ll_product * pll[q];
    else
      t += cm.sparse_product * (ptot - ph[q]);
    if (log) {
      snprintf(buf, sizeof buf, "H %d tdpbuud %.3g heavy %.3g hl_rows %.3g ll %.3g grid_rows %.3g grid_nnz %.3g model %.3f s\n",
               cand[q], tdp[q], ph[q], lr[q], pll[q], gr[q], gz[q], t);
      *log += buf;
    }
    if (t < bestT) { bestT = t; best = cand[q]; }
  }
  return best;
}

bool dense_fits_int32(const Csr& X, int H) {
  if (X.val.empty()) return true;
  std::vector<uint64_t> colsum(std::min(H, X.n_cols), 0);
  uint64_t maxv = 0;
  for (int32_t r = 0; r < X.n_rows; r++)
    for (int64_t e = X.ptr[r]; e < X.ptr[r + 1] && X.idx[e] < H; e++) {
      colsum[X.idx[e]] += X.val[e];
      maxv = std::max<uint64_t>(maxv, X.val[e]);
    }
  uint64_t m = 0;
  for (auto c : colsum) m = std::max(m, c);
  return m * maxv < (1ull << 31);
}

}
}
