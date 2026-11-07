#pragma once
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "tessera.hpp"

namespace tessera {

enum class PairMode : uint8_t {
  FULL,
  LT,
  LE,
  NE
};
inline bool has_diag(PairMode m) { return m == PairMode::FULL || m == PairMode::LE; }
inline bool both_orders(PairMode m) { return m == PairMode::FULL || m == PairMode::NE; }

struct QueryPart {
  std::vector<int64_t> k, x;
  std::vector<double> v;
  std::vector<int64_t> iv;
  std::vector<std::vector<double>> sum_v;
  std::vector<std::vector<int64_t>> sum_iv;
  std::vector<uint64_t> sum_null;
};

enum class GroupOf : uint8_t { EACH, LOWER, HIGHER, FIRST, SECOND };

enum class Similarity : uint8_t { NONE, COSINE, JACCARD, DICE, OVERLAP };

struct QuerySpec {
  bool has_value = false;
  PairMode mode = PairMode::FULL;
  uint64_t min_value = 0;
  int64_t topk = -1;
  int64_t group_topk = -1;
  GroupOf group_of = GroupOf::EACH;
  int many_pair_op = 0;
  int threads = 1;
  int64_t frontier = -1;
  bool reorder = true;
  bool verbose = false;
  size_t out_budget = 0;
  std::string spill_dir;
  bool spill = true;
  bool spill_mkdir = false;
  uint64_t spill_limit = 0;
  size_t out_chunk = 0;
  int32_t topk_first = 0;
  bool (*should_stop)(void*) = nullptr;
  void* stop_ctx = nullptr;
  bool release_threads = true;
  Similarity similarity = Similarity::NONE;
  double min_sim = 0;
  bool distinct_keys = false;
  bool (*pair_pass)(void* ctx, int64_t a, int64_t b) = nullptr;
  void* pair_ctx = nullptr;
  __int128 prod_lo = 0, prod_hi = 0;
  bool float_errors = false;
  bool float_accum = false;
  double sum_limit = 0;
};

constexpr uint64_t ROW_VALUE_OVERFLOW = 0x7ff8000000000001ull, ROW_VALUE_UNDERFLOW = 0x7ff8000000000002ull;

struct Cancelled : std::runtime_error {
  Cancelled() : std::runtime_error("tessera: query cancelled") {}
};

struct QueryResult {
  std::vector<int64_t> orig;
  std::vector<Cells> out;
  bool general = false;
  bool wide = false;
  bool int_values = false;
  __int128 prod_min = 0, prod_max = 0, cell_min = 0, cell_max = 0;
  std::vector<uint8_t> side;
  std::vector<std::vector<__int128>> out2;
  bool general2 = false;
  struct SumResult {
    size_t at = 1;
    bool general = false, int_values = false;
    __int128 prod_min = 0, prod_max = 0, cell_min = 0, cell_max = 0;
  };
  size_t stride = 1;
  std::vector<SumResult> sums;
  size_t null_at = 0;
  int float_errors = 0;
};

void run_query(std::vector<std::unique_ptr<QueryPart>>& parts, const QuerySpec& q, QueryResult& r);

void run_query_cross(std::vector<std::unique_ptr<QueryPart>>& parts_x, std::vector<std::unique_ptr<QueryPart>>& parts_y,
                     const QuerySpec& spec, QueryResult& r);

void run_query_two(std::vector<std::unique_ptr<QueryPart>>& parts, std::vector<std::unique_ptr<QueryPart>>* parts_y,
                   const QuerySpec& spec, QueryResult& r);

void run_query_many(std::vector<std::unique_ptr<QueryPart>>& parts, std::vector<std::unique_ptr<QueryPart>>& parts_y,
                    size_t n_sums, const QuerySpec& spec, QueryResult& r);

double cell_value(uint64_t s, bool general);
inline __int128 cell_value128(const Cells& c, size_t q, bool wide) {
  if (!wide) return (__int128)(int64_t)c.data[q].s;
  return (__int128)(((unsigned __int128)c.data[2 * q + 1].s << 64) | c.data[2 * q].s);
}
inline size_t cell_count(const Cells& c, bool wide) { return wide ? c.n / 2 : c.n; }
int overflow_flags(const QueryResult& r, int product_bits, int sum_bits);
int overflow_flags(const QueryResult::SumResult& r, int product_bits, int sum_bits);

class StrDict {
 public:
  struct Bytes {
    const char* p;
    size_t n;
    const char* data() const { return p; }
    size_t size() const { return n; }
    char operator[](size_t i) const { return p[i]; }
  };
  int64_t code(const char* s, size_t len);
  std::vector<int64_t> finish();
  size_t size() const { return off_.size() - 1; }
  Bytes str(int64_t c) const { return Bytes{bytes_.data() + off_[(size_t)c], (size_t)(off_[(size_t)c + 1] - off_[(size_t)c])}; }
  bool set_order(const std::vector<int64_t>& rank);

 private:
  void grow();
  std::vector<char> bytes_;
  std::vector<uint64_t> off_{0};
  std::vector<uint64_t> slot_;
  std::vector<int64_t> rank_;
};

}
