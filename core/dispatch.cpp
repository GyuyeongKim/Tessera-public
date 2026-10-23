#include <cpuid.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "tessera.hpp"

namespace tessera {

#define TESSERA_VARIANT_DECLS                                                                                     \
  Checksum xtx(const Csr& X, const Options& opt, Stats* st);                                                       \
  int choose_frontier(const Csr& X, int threads, const CostModel& cm, std::string* log, bool hl);                  \
  int choose_frontier_cross(const Csr& X, const std::vector<uint8_t>& side, int threads, const CostModel& cm,      \
                            std::string* log);                                                                     \
  bool dense_fits_int32(const Csr& X, int H);                                                                      \
  std::string u128(unsigned __int128 v);
namespace amx { TESSERA_VARIANT_DECLS }
namespace vnni { TESSERA_VARIANT_DECLS }
namespace avx512 { TESSERA_VARIANT_DECLS }
namespace avx2 { TESSERA_VARIANT_DECLS }

namespace {

uint64_t xgetbv0() {
  uint32_t a, d;
  __asm__ volatile("xgetbv" : "=a"(a), "=d"(d) : "c"(0));
  return (uint64_t)d << 32 | a;
}

Isa detect() {
  unsigned a, b, c, d;
  if (!__get_cpuid(1, &a, &b, &c, &d) || !(c & (1u << 27))) return Isa::AVX2;
  const uint64_t xcr0 = xgetbv0();
  if (!__get_cpuid_count(7, 0, &a, &b, &c, &d)) return Isa::AVX2;
  const bool avx512 = (b & (1u << 16)) && (b & (1u << 17)) && (b & (1u << 28)) && (b & (1u << 30)) && (b & (1u << 31)) &&
                      (xcr0 & 0xe6) == 0xe6;
  const bool amx = avx512 && (d & (1u << 24)) && (d & (1u << 25)) && (c & (1u << 1)) && (c & (1u << 8)) &&
                   (xcr0 & (3ull << 17)) == (3ull << 17) &&
                   syscall(SYS_arch_prctl, 0x1023 , 18 ) == 0;
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
  Isa want = strcmp(e, "amx") == 0 ? Isa::AMX : strcmp(e, "avx512") == 0 ? Isa::AVX512 : Isa::AVX2;
  if ((int)want > (int)best) {
    fprintf(stderr, "tessera: TESSERA_ISA=%s is not supported here, using %s\n", e, isa_name(best));
    return best;
  }
  return want;
}

}

Isa active_isa() {
  static const Isa isa = choose();
  return isa;
}

const char* isa_name(Isa i) {
  return i == Isa::AMX ? "amx" : i == Isa::VNNI ? "vnni" : i == Isa::AVX512 ? "avx512" : "avx2";
}

Checksum xtx(const Csr& X, const Options& opt, Stats* st) {
  switch (active_isa()) {
    case Isa::AMX: return amx::xtx(X, opt, st);
    case Isa::VNNI: return vnni::xtx(X, opt, st);
    case Isa::AVX512: return avx512::xtx(X, opt, st);
    default: return avx2::xtx(X, opt, st);
  }
}

CostModel vnni_model(const CostModel& cm) {
  CostModel v = cm;
  const char* e = getenv("TESSERA_VNNI_TDP");
  v.tdpbuud = e && *e ? atof(e) : cm.vnni_tdpbuud;
  e = getenv("TESSERA_VNNI_HL");
  v.hl_row_group = e && *e ? atof(e) : cm.vnni_hl_row_group;
  return v;
}

int choose_frontier(const Csr& X, int threads, const CostModel& cm, std::string* log, bool hl) {
  switch (active_isa()) {
    case Isa::AMX: return amx::choose_frontier(X, threads, cm, log, hl);
    case Isa::VNNI: return vnni::choose_frontier(X, threads, vnni_model(cm), log, hl);
    default: return 0;
  }
}

int choose_frontier_cross(const Csr& X, const std::vector<uint8_t>& side, int threads, const CostModel& cm,
                          std::string* log) {
  switch (active_isa()) {
    case Isa::AMX: return amx::choose_frontier_cross(X, side, threads, cm, log);
    case Isa::VNNI: return vnni::choose_frontier_cross(X, side, threads, vnni_model(cm), log);
    default: return 0;
  }
}

bool dense_fits_int32(const Csr& X, int H) { return avx2::dense_fits_int32(X, H); }

std::string u128(unsigned __int128 v) { return avx2::u128(v); }

}
