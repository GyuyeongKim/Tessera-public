#if defined(__x86_64__)
#include <cpuid.h>
#endif
#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "tessera.hpp"
#include "tessera_c.h"

namespace tessera {

#define TESSERA_VARIANT_DECLS                                                                                     \
  Checksum xtx(const Csr& X, const Options& opt, Stats* st);                                                       \
  int choose_frontier(const Csr& X, int threads, const CostModel& cm, std::string* log, bool hl);                  \
  int choose_frontier_cross(const Csr& X, const std::vector<uint8_t>& side, int threads, const CostModel& cm,      \
                            std::string* log);                                                                     \
  bool dense_fits_int32(const Csr& X, int H);                                                                      \
  std::string u128(unsigned __int128 v);
#if defined(__x86_64__)
namespace amx { TESSERA_VARIANT_DECLS }
namespace vnni { TESSERA_VARIANT_DECLS }
namespace avx512 { TESSERA_VARIANT_DECLS }
namespace avx2 { TESSERA_VARIANT_DECLS }
namespace base = avx2;
#else
namespace portable { TESSERA_VARIANT_DECLS }
namespace base = portable;
#endif

namespace {

#if defined(__x86_64__)
uint64_t xgetbv0() {
  uint32_t a, d;
  __asm__ volatile("xgetbv" : "=a"(a), "=d"(d) : "c"(0));
  return (uint64_t)d << 32 | a;
}

bool v3_ok() {
  unsigned a, b, c, d;
  const unsigned ecx1 = 1u << 0 | 1u << 9 | 1u << 12 | 1u << 13 | 1u << 19 | 1u << 20 | 1u << 22 | 1u << 23 | 1u << 26 |
                        1u << 27 | 1u << 28 | 1u << 29;
  if (!__get_cpuid(1, &a, &b, &c, &d) || (c & ecx1) != ecx1 || (xgetbv0() & 6) != 6) return false;
  const unsigned ebx7 = 1u << 3 | 1u << 5 | 1u << 8;
  if (!__get_cpuid_count(7, 0, &a, &b, &c, &d) || (b & ebx7) != ebx7) return false;
  return __get_cpuid(0x80000001, &a, &b, &c, &d) && (c & (1u << 0 | 1u << 5)) == (1u << 0 | 1u << 5);
}

bool amx_permitted() {
#if defined(__linux__)
  return syscall(SYS_arch_prctl, 0x1023 , 18 ) == 0;
#else
  return false;
#endif
}

Isa detect() {
  unsigned a, b, c, d;
  if (!v3_ok()) return Isa::AVX2;
  __get_cpuid(1, &a, &b, &c, &d);
  const uint64_t xcr0 = xgetbv0();
  if (!__get_cpuid_count(7, 0, &a, &b, &c, &d)) return Isa::AVX2;
  const bool avx512 = (b & (1u << 16)) && (b & (1u << 17)) && (b & (1u << 28)) && (b & (1u << 30)) && (b & (1u << 31)) &&
                      (xcr0 & 0xe6) == 0xe6;
  const bool amx = avx512 && (d & (1u << 24)) && (d & (1u << 25)) && (c & (1u << 1)) && (c & (1u << 8)) &&
                   (xcr0 & (3ull << 17)) == (3ull << 17) &&
                   amx_permitted();
  return amx ? Isa::AMX : avx512 ? Isa::AVX512 : Isa::AVX2;
}

bool vnni_ok() {
  unsigned a, b, c, d;
  return __get_cpuid_count(7, 0, &a, &b, &c, &d) && (c & (1u << 11)) && (c & (1u << 1)) && (c & (1u << 8)) &&
         (b & (1u << 8));
}

Isa choose() {
  Isa best = detect();
  const char* e = getenv("TESSERA_ISA");
  if (!e || !*e) return best;
  if (strcmp(e, "vnni") == 0) {
    if ((int)best >= (int)Isa::AVX512 && vnni_ok()) return Isa::VNNI;
    fprintf(stderr, "tessera: TESSERA_ISA=vnni is not supported here, using %s\n", isa_name(best));
    return best;
  }
  const bool known = strcmp(e, "amx") == 0 || strcmp(e, "avx512") == 0 || strcmp(e, "avx2") == 0;
  if (!known) {
    fprintf(stderr, "tessera: TESSERA_ISA=%s is not one of amx, vnni, avx512, avx2, using %s\n", e, isa_name(best));
    return best;
  }
  Isa want = strcmp(e, "amx") == 0 ? Isa::AMX : strcmp(e, "avx512") == 0 ? Isa::AVX512 : Isa::AVX2;
  if ((int)want > (int)best) {
    fprintf(stderr, "tessera: TESSERA_ISA=%s is not supported here, using %s\n", e, isa_name(best));
    return best;
  }
  return want;
}
#else
Isa choose() { return Isa::PORTABLE; }
#endif

}

bool cpu_supported() {
#if defined(__x86_64__)
  static const bool ok = v3_ok();
  return ok;
#else
  return true;
#endif
}

Isa active_isa() {
  static const Isa isa = choose();
  return isa;
}

const char* isa_name(Isa i) {
  return i == Isa::AMX ? "amx" : i == Isa::VNNI ? "vnni" : i == Isa::AVX512 ? "avx512" : i == Isa::PORTABLE ? "portable"
                                                                                                    : "avx2";
}

Checksum xtx(const Csr& X, const Options& opt, Stats* st) {
#if defined(__x86_64__)
  switch (active_isa()) {
    case Isa::AMX: return amx::xtx(X, opt, st);
    case Isa::VNNI: return vnni::xtx(X, opt, st);
    case Isa::AVX512: return avx512::xtx(X, opt, st);
    default: return avx2::xtx(X, opt, st);
  }
#else
  return portable::xtx(X, opt, st);
#endif
}

CostModel vnni_model(const CostModel& cm) {
  auto env = [](const char* name, double def) {
    const char* e = getenv(name);
    if (!e || !*e) return def;
    char* end = nullptr;
    const double v = strtod(e, &end);
    return *end == '\0' && v > 0 && std::isfinite(v) ? v : def;
  };
  CostModel v = cm;
  v.tdpbuud = env("TESSERA_VNNI_TDP", cm.vnni_tdpbuud);
  v.hl_row_group = env("TESSERA_VNNI_HL", cm.vnni_hl_row_group);
  return v;
}

CostModel env_cost_model() {
  CostModel cm;
  const char* e = getenv("TESSERA_COST_MODEL");
  if (!e || !*e) return cm;
  static const struct {
    const char* name;
    double CostModel::*field;
  } fields[] = {{"tdpbuud", &CostModel::tdpbuud},       {"block_pair", &CostModel::block_pair},
                {"sparse_product", &CostModel::sparse_product}, {"hl_row_group", &CostModel::hl_row_group},
                {"ll_product", &CostModel::ll_product}, {"grid_row", &CostModel::grid_row},
                {"grid_nnz", &CostModel::grid_nnz}};
  static std::atomic<bool> warned{false};
  const std::string s(e);
  size_t i = 0;
  while (i < s.size()) {
    size_t j = s.find_first_of(", \t", i);
    if (j == std::string::npos) j = s.size();
    const std::string item = s.substr(i, j - i);
    i = j + 1;
    if (item.empty()) continue;
    bool ok = false;
    const size_t eq = item.find('=');
    if (eq != std::string::npos) {
      const std::string name = item.substr(0, eq), num = item.substr(eq + 1);
      char* end = nullptr;
      const double v = strtod(num.c_str(), &end);
      if (!num.empty() && *end == '\0' && v > 0 && std::isfinite(v))
        for (const auto& f : fields)
          if (name == f.name) {
            cm.*(f.field) = v;
            ok = true;
          }
    }
    if (!ok && !warned.exchange(true)) fprintf(stderr, "tessera: TESSERA_COST_MODEL: skipped '%s'\n", item.c_str());
  }
  return cm;
}

int choose_frontier(const Csr& X, int threads, const CostModel& cm, std::string* log, bool hl) {
#if defined(__x86_64__)
  switch (active_isa()) {
    case Isa::AMX: return amx::choose_frontier(X, threads, cm, log, hl);
    case Isa::VNNI: return vnni::choose_frontier(X, threads, vnni_model(cm), log, hl);
    default: return 0;
  }
#else
  return 0;
#endif
}

int choose_frontier_cross(const Csr& X, const std::vector<uint8_t>& side, int threads, const CostModel& cm,
                          std::string* log) {
#if defined(__x86_64__)
  switch (active_isa()) {
    case Isa::AMX: return amx::choose_frontier_cross(X, side, threads, cm, log);
    case Isa::VNNI: return vnni::choose_frontier_cross(X, side, threads, vnni_model(cm), log);
    default: return 0;
  }
#else
  return 0;
#endif
}

bool dense_fits_int32(const Csr& X, int H) { return base::dense_fits_int32(X, H); }

std::string u128(unsigned __int128 v) { return base::u128(v); }

}

extern "C" int tessera_c_supported(void) { return tessera::cpu_supported() ? 1 : 0; }
