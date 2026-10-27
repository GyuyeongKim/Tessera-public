#pragma once
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "tessera.hpp"

namespace tessera {

enum class PairMode : uint8_t {
  FULL,
  LT,
  LE
};

struct QueryPart {
  std::vector<int64_t> k, x;
  std::vector<double> v;
  std::vector<int64_t> iv;
};

enum class Similarity : uint8_t { NONE, COSINE, JACCARD, DICE, OVERLAP };

struct QuerySpec {
  bool has_value = false;
  PairMode mode = PairMode::FULL;
  uint64_t min_value = 0;
  int64_t topk = -1;
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
};

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
};

void run_query(std::vector<std::unique_ptr<QueryPart>>& parts, const QuerySpec& q, QueryResult& r);

void run_query_cross(std::vector<std::unique_ptr<QueryPart>>& parts_x, std::vector<std::unique_ptr<QueryPart>>& parts_y,
                     const QuerySpec& spec, QueryResult& r);

double cell_value(uint64_t s, bool general);
inline __int128 cell_value128(const Cells& c, size_t q, bool wide) {
  if (!wide) return (__int128)(int64_t)c.data[q].s;
  return (__int128)(((unsigned __int128)c.data[2 * q + 1].s << 64) | c.data[2 * q].s);
}
inline size_t cell_count(const Cells& c, bool wide) { return wide ? c.n / 2 : c.n; }
int overflow_flags(const QueryResult& r, int product_bits, int sum_bits);

class StrDict {
 public:
  int64_t code(const char* s, size_t len);
  std::vector<int64_t> finish();
  size_t size() const { return strs_.size(); }
  const std::string& str(int64_t c) const { return *strs_[(size_t)c]; }

 private:
  std::unordered_map<std::string, int64_t> map_;
  std::vector<const std::string*> strs_;
};

}
