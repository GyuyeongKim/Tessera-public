#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace tessera {

constexpr const char* kVersion = "1.2.1";

struct Csr {
  int32_t n_rows = 0, n_cols = 0;
  std::vector<int64_t> ptr;
  std::vector<int32_t> idx;
  std::vector<uint8_t> val;
  std::vector<uint32_t> ival;
  std::vector<double> wval;
  std::vector<int64_t> xval;
  std::vector<__int128> xval_wide;
};

struct Triple {
  int32_t i, j;
  uint64_t s;
};
struct ChunkFree {
  size_t bytes;
  std::shared_ptr<void> map;
  explicit ChunkFree(size_t b = 0, std::shared_ptr<void> m = nullptr) : bytes(b), map(std::move(m)) {}
  void operator()(Triple* p) const;
};
using ChunkPtr = std::unique_ptr<Triple[], ChunkFree>;
struct Cells {
  ChunkPtr data;
  size_t n = 0;
  int64_t file_off = -1;
};

struct OutSpace {
  size_t budget = 0;
  std::string dir;
  std::atomic<size_t> heap{0};
  std::atomic<uint64_t> spilled{0}, spill_failed{0};
  size_t chunk_cells = 0;
  struct SpillFile {
    int fd = -1;
    std::atomic<int64_t> end{0};
  };
  std::unique_ptr<SpillFile[]> files;
  int n_files = 0;
  bool file_failed = false;
  std::atomic<bool> file_lock{false};
  std::string spill_dir;
  bool spill = true, mkdir_dir = false;
  uint64_t spill_limit = 0;
  std::atomic<uint64_t> spill_bytes{0};
  bool limit_hit = false;
  std::unique_ptr<char[]> names;
  size_t name_len = 0;
  std::atomic<bool> failed{false}, oom{false};
  char error[512] = {0};
  int file_errno = 0;
  void no_memory() {
    oom.store(true, std::memory_order_relaxed);
    failed.store(true, std::memory_order_relaxed);
  }
  void spill_fail(const char* what, int err);
  void lanes(int n);
  int file(int lane);
  ChunkPtr alloc(size_t cells);
  void settle(Cells& c, ChunkPtr& spare, int lane);
  bool map_spilled(std::vector<Cells>& chunks, int lane);
  void release(ChunkPtr& buf);
  ~OutSpace();
};

enum class DenseMode { GRID, SQUARE_F32, SQUARE_S8 };

struct Options {
  int threads = 16;
  int H = 0;
  bool reorder = true;
  int co_order = 0;
  int co_mode = 1;
  int pair_frontier = 0;
  double pair_tdp_ns = 1.3, pair_prod_ns = 0.25;
  bool dense = true;
  bool sparse = true;
  bool checksum = true;
  int group = 256;
  int unit = 4;
  int range_panels = 0;
  DenseMode dense_mode = DenseMode::GRID;
  int copy_block = 0;
  bool hl = true;
  int hl_frontier = 0;
  bool hl_order = false;
  std::vector<Cells>* out = nullptr;
  uint64_t min_value = 0;
  size_t out_budget = 0;
  std::string spill_dir;
  bool spill = true;
  bool spill_mkdir = false;
  uint64_t spill_limit = 0;
  size_t out_chunk = 0;
  bool (*should_stop)(void*) = nullptr;
  void* stop_ctx = nullptr;
  int cross_hx = -1;
  int cross_lx_end = 0;
  int sim_measure = 0;
  double min_sim = 0;
  bool sim_diag = false;
  const double* sim_n = nullptr;
  const int32_t* sim_lo = nullptr;
  bool exact_wide = false;
  int exact_track_from = 0;
  bool exact_track_diag = true;
};

inline double similarity(int measure, double c, double ni, double nj) {
  switch (measure) {
    case 1: return c / __builtin_sqrt(ni * nj);
    case 2: return c / (ni + nj - c);
    case 3: return 2 * c / (ni + nj);
    default: return c / (ni < nj ? ni : nj);
  }
}

struct Stats {
  double t_order = 0, t_panels = 0, t_dense = 0, t_sparse = 0, t_total = 0;
  uint64_t grid_rows = 0, chunks = 0, blocks = 0, panels = 0, block_pairs = 0, tdpbuud = 0;
  uint64_t sparse_products = 0;
  uint64_t pair_prefix = 0;
  uint64_t ranges = 0, c_reloads = 0;
  double t_colidx = 0, t_hl_build = 0, t_hl = 0;
  uint64_t hl_tdpbuud = 0, hl_panels = 0, hl_rows = 0;
  uint64_t spilled_chunks = 0, spill_failed = 0;
  bool cancelled = false;
  bool out_of_memory = false;
  std::string spill_error;
  bool spill_limit_hit = false;
  bool cell_any = false;
  __int128 cell_min = 0, cell_max = 0;
};

struct Checksum {
  uint64_t cells = 0;
  unsigned __int128 sum = 0, pos = 0;
  void add(const Checksum& o) { cells += o.cells; sum += o.sum; pos += o.pos; }
};

Checksum xtx(const Csr& X, const Options& opt, Stats* st);

struct CostModel {
  double tdpbuud = 1.25e-9;
  double block_pair = 6.0e-7;
  double sparse_product = 0.25e-9;
  double hl_row_group = 0.7e-9;
  double ll_product = 0.5e-9;
  double grid_row = 1.8e-8;
  double grid_nnz = 2.4e-9;
  double vnni_tdpbuud = 6.758e-9;
  double vnni_hl_row_group = 8.976e-10;
};
CostModel env_cost_model();
int choose_frontier(const Csr& X, int threads, const CostModel& cm = CostModel(), std::string* log = nullptr,
                    bool hl = false);
int choose_frontier_cross(const Csr& X, const std::vector<uint8_t>& side, int threads, const CostModel& cm = CostModel(),
                          std::string* log = nullptr);
bool dense_fits_int32(const Csr& X, int H);
std::string u128(unsigned __int128 v);

struct OutOfMemory : std::runtime_error {
  explicit OutOfMemory(const std::string& m) : std::runtime_error(m) {}
};
struct SpillError : std::runtime_error {
  bool limit;
  SpillError(const std::string& m, bool lim) : std::runtime_error(m), limit(lim) {}
};

enum class Isa { AVX2 = 0, AVX512 = 1, VNNI = 2, AMX = 3, PORTABLE = 4 };
Isa active_isa();
const char* isa_name(Isa i);
bool cpu_supported();

}
