#include <jni.h>
#include <stdint.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "tessera_c.h"

namespace {

const jint SLICE = 1 << 20;

thread_local int last_status = TESSERA_C_OK;

void throw_oom(JNIEnv *env, const char *what) {
  jclass c = env->FindClass("java/lang/OutOfMemoryError");
  if (c) env->ThrowNew(c, what);
}

void throw_state(JNIEnv *env, const char *what) {
  jclass c = env->FindClass("java/lang/IllegalStateException");
  if (c) env->ThrowNew(c, what);
}

bool holds(JNIEnv *env, jarray a, long long want, bool optional) {
  if (!a) return optional;
  return want >= 0 && (long long)env->GetArrayLength(a) >= want;
}

struct Pinned {
  JNIEnv *env;
  jarray arr[6];
  void *ptr[6];
  int mode[6];
  int n = 0;
  bool good = true;
  explicit Pinned(JNIEnv *e) : env(e) {}
  void *pin(jarray a, int m) {
    if (!a || !good) return nullptr;
    void *p = env->GetPrimitiveArrayCritical(a, nullptr);
    if (!p) {
      good = false;
      return nullptr;
    }
    arr[n] = a;
    ptr[n] = p;
    mode[n] = m;
    n++;
    return p;
  }
  bool ok() const { return good; }
  ~Pinned() {
    for (int i = n - 1; i >= 0; i--) env->ReleasePrimitiveArrayCritical(arr[i], ptr[i], mode[i]);
  }
};

template <class V, class Add>
jint add_slices(JNIEnv *env, jlongArray k, jlongArray x, V v, jint n, Add add) {
  for (jint o = 0; o < n; o += SLICE) {
    const jint m = std::min(SLICE, n - o);
    int rc;
    {
      Pinned p(env);
      jlong *kp = (jlong *)p.pin(k, JNI_ABORT), *xp = (jlong *)p.pin(x, JNI_ABORT);
      void *vp = p.pin(v, JNI_ABORT);
      if (!p.ok()) rc = -2;
      else rc = add(kp + o, xp + o, vp, o, m);
    }
    if (rc == -2) {
      throw_oom(env, "tessera: cannot pin the rows");
      return -1;
    }
    if (rc != 0) return rc;
  }
  return 0;
}

jint add_rows(JNIEnv *env, jlong h, jlongArray k, jlongArray x, jdoubleArray v, jint n, bool second) {
  tessera_query_c *q = (tessera_query_c *)h;
  return add_slices(env, k, x, v, n, [&](jlong *kp, jlong *xp, void *vp, jint o, jint m) {
    const double *w = vp ? (const double *)vp + o : nullptr;
    return second ? tessera_c_add_y(q, (const int64_t *)kp, (const int64_t *)xp, w, (size_t)m)
                  : tessera_c_add(q, (const int64_t *)kp, (const int64_t *)xp, w, (size_t)m);
  });
}

jint add_rows_i64(JNIEnv *env, jlong h, jlongArray k, jlongArray x, jlongArray v, jint n, bool second) {
  tessera_query_c *q = (tessera_query_c *)h;
  return add_slices(env, k, x, v, n, [&](jlong *kp, jlong *xp, void *vp, jint o, jint m) {
    const int64_t *w = vp ? (const int64_t *)vp + o : nullptr;
    return second ? tessera_c_add_y_i64(q, (const int64_t *)kp, (const int64_t *)xp, w, (size_t)m)
                  : tessera_c_add_i64(q, (const int64_t *)kp, (const int64_t *)xp, w, (size_t)m);
  });
}

typedef size_t (*strings_fn)(const tessera_query_c *, char *, size_t, int64_t *, size_t, size_t *);

template <bool BYTES>
jarray strings(JNIEnv *env, jlong h, strings_fn f) {
  size_t total = 0;
  const size_t d = f((const tessera_query_c *)h, nullptr, 0, nullptr, 0, &total);
  if (total > (size_t)INT32_MAX || d + 1 > (size_t)INT32_MAX) {
    throw_state(env, "tessera: the group keys' strings exceed a Java array (2^31 - 1 bytes or strings)");
    return nullptr;
  }
  try {
    std::vector<char> b(total);
    std::vector<int64_t> o(d + 1);
    f((const tessera_query_c *)h, b.data(), total, o.data(), d + 1, &total);
    if (BYTES) {
      jbyteArray r = env->NewByteArray((jsize)total);
      if (r) env->SetByteArrayRegion(r, 0, (jsize)total, (const jbyte *)b.data());
      return r;
    }
    jlongArray r = env->NewLongArray((jsize)(d + 1));
    if (r) env->SetLongArrayRegion(r, 0, (jsize)(d + 1), (const jlong *)o.data());
    return r;
  } catch (const std::bad_alloc &) {
    throw_oom(env, "tessera: the group keys' strings");
    return nullptr;
  }
}

}

extern "C" {

JNIEXPORT jint JNICALL Java_tessera_spark_TesseraNative_apiVersion(JNIEnv *, jclass) { return tessera_c_api_version(); }

JNIEXPORT jboolean JNICALL Java_tessera_spark_TesseraNative_supported(JNIEnv *, jclass) {
  return tessera_c_supported() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL Java_tessera_spark_TesseraNative_version(JNIEnv *env, jclass) {
  char v[64];
  snprintf(v, sizeof v, "%s (%s)", tessera_c_version(), tessera_c_isa());
  return env->NewStringUTF(v);
}

JNIEXPORT jlong JNICALL Java_tessera_spark_TesseraNative_create(JNIEnv *, jclass, jboolean has_value) {
  return (jlong)tessera_c_new(has_value ? 1 : 0);
}

JNIEXPORT jint JNICALL Java_tessera_spark_TesseraNative_add(JNIEnv *env, jclass, jlong h, jlongArray k, jlongArray x,
                                                            jdoubleArray v, jint n) {
  return add_rows(env, h, k, x, v, n, false);
}

JNIEXPORT jint JNICALL Java_tessera_spark_TesseraNative_addI64(JNIEnv *env, jclass, jlong h, jlongArray k, jlongArray x,
                                                               jlongArray v, jint n) {
  return add_rows_i64(env, h, k, x, v, n, false);
}

JNIEXPORT jint JNICALL Java_tessera_spark_TesseraNative_addYI64(JNIEnv *env, jclass, jlong h, jlongArray k,
                                                                jlongArray y, jlongArray w, jint n) {
  return add_rows_i64(env, h, k, y, w, n, true);
}

JNIEXPORT jint JNICALL Java_tessera_spark_TesseraNative_overflow(JNIEnv *, jclass, jlong h, jint product_bits,
                                                                 jint sum_bits) {
  return tessera_c_overflow((const tessera_query_c *)h, product_bits, sum_bits);
}

JNIEXPORT jlong JNICALL Java_tessera_spark_TesseraNative_createCross(JNIEnv *, jclass, jboolean has_value) {
  return (jlong)tessera_c_new_cross(has_value ? 1 : 0);
}

JNIEXPORT jint JNICALL Java_tessera_spark_TesseraNative_addY(JNIEnv *env, jclass, jlong h, jlongArray k, jlongArray y,
                                                             jdoubleArray w, jint n) {
  return add_rows(env, h, k, y, w, n, true);
}

JNIEXPORT jlong JNICALL Java_tessera_spark_TesseraNative_dictNew(JNIEnv *, jclass) { return (jlong)tessera_dict_new(); }

JNIEXPORT jint JNICALL Java_tessera_spark_TesseraNative_dictCode(JNIEnv *env, jclass, jlong d, jbyteArray bytes,
                                                                 jintArray offs, jint n, jlongArray out) {
  for (jint s = 0; s < n; s += SLICE) {
    const jint m = std::min(SLICE, n - s);
    int rc = 0;
    {
      Pinned p(env);
      jbyte *b = (jbyte *)p.pin(bytes, JNI_ABORT);
      jint *o = (jint *)p.pin(offs, JNI_ABORT);
      jlong *c = (jlong *)p.pin(out, 0);
      if (!p.ok()) {
        rc = -2;
      } else {
        for (jint i = s; i < s + m && rc == 0; i++) {
          c[i] = tessera_dict_code((tessera_dict_c *)d, (const char *)b + o[i], (size_t)(o[i + 1] - o[i]));
          if (c[i] < 0) rc = -1;
        }
      }
    }
    if (rc == -2) {
      throw_oom(env, "tessera: cannot pin the strings");
      return -1;
    }
    if (rc != 0) return rc;
  }
  return 0;
}

JNIEXPORT void JNICALL Java_tessera_spark_TesseraNative_dictFree(JNIEnv *, jclass, jlong d) {
  tessera_dict_free((tessera_dict_c *)d);
}

JNIEXPORT void JNICALL Java_tessera_spark_TesseraNative_setXDict(JNIEnv *, jclass, jlong h, jlong d) {
  tessera_c_set_xdict((tessera_query_c *)h, (tessera_dict_c *)d);
}

JNIEXPORT jbyteArray JNICALL Java_tessera_spark_TesseraNative_xStringBytes(JNIEnv *env, jclass, jlong h) {
  return (jbyteArray)strings<true>(env, h, tessera_c_xstrings);
}

JNIEXPORT jlongArray JNICALL Java_tessera_spark_TesseraNative_xStringOffsets(JNIEnv *env, jclass, jlong h) {
  return (jlongArray)strings<false>(env, h, tessera_c_xstrings);
}

JNIEXPORT void JNICALL Java_tessera_spark_TesseraNative_setYDict(JNIEnv *, jclass, jlong h, jlong d) {
  tessera_c_set_ydict((tessera_query_c *)h, (tessera_dict_c *)d);
}

JNIEXPORT jbyteArray JNICALL Java_tessera_spark_TesseraNative_yStringBytes(JNIEnv *env, jclass, jlong h) {
  return (jbyteArray)strings<true>(env, h, tessera_c_ystrings);
}

JNIEXPORT jlongArray JNICALL Java_tessera_spark_TesseraNative_yStringOffsets(JNIEnv *env, jclass, jlong h) {
  return (jlongArray)strings<false>(env, h, tessera_c_ystrings);
}

JNIEXPORT jstring JNICALL Java_tessera_spark_TesseraNative_run(JNIEnv *env, jclass, jlong h, jint mode,
                                                               jboolean col0_smaller, jlong min_value, jlong topk,
                                                               jint threads, jlong frontier, jboolean verbose) {
  char err[256];
  last_status = tessera_c_run((tessera_query_c *)h, mode, col0_smaller ? 1 : 0, (uint64_t)min_value, topk, threads,
                              frontier, verbose ? 1 : 0, err, sizeof err);
  return last_status == TESSERA_C_OK ? nullptr : env->NewStringUTF(err);
}

JNIEXPORT jint JNICALL Java_tessera_spark_TesseraNative_lastStatus(JNIEnv *, jclass) { return last_status; }

JNIEXPORT void JNICALL Java_tessera_spark_TesseraNative_setSimilarity(JNIEnv *, jclass, jlong h, jint measure,
                                                                      jdouble min_sim) {
  tessera_c_set_similarity((tessera_query_c *)h, measure, min_sim);
}

JNIEXPORT void JNICALL Java_tessera_spark_TesseraNative_setDistinctKeys(JNIEnv *, jclass, jlong h, jboolean on) {
  tessera_c_set_distinct_keys((tessera_query_c *)h, on ? 1 : 0);
}

JNIEXPORT void JNICALL Java_tessera_spark_TesseraNative_setGroupTop(JNIEnv *, jclass, jlong h, jlong k, jint of) {
  tessera_c_set_group_top((tessera_query_c *)h, k, of);
}

JNIEXPORT void JNICALL Java_tessera_spark_TesseraNative_setSumLimit(JNIEnv *, jclass, jlong h, jdouble limit) {
  tessera_c_set_sum_limit((tessera_query_c *)h, limit);
}

JNIEXPORT void JNICALL Java_tessera_spark_TesseraNative_setMany(JNIEnv *, jclass, jlong h, jint n, jint pair_op) {
  tessera_c_set_many((tessera_query_c *)h, (size_t)n, pair_op);
}

JNIEXPORT jint JNICALL Java_tessera_spark_TesseraNative_addMany(JNIEnv *env, jclass, jlong h, jboolean second, jlongArray k,
                                                                jlongArray x, jdoubleArray v, jint n) {
  int rc = -1;
  {
    Pinned p(env);
    jlong *a = (jlong *)p.pin(k, 0), *b = (jlong *)p.pin(x, 0);
    jdouble *d = (jdouble *)p.pin(v, 0);
    if (p.ok()) {
      rc = tessera_c_add_many((tessera_query_c *)h, second ? 1 : 0, (int64_t *)a, (int64_t *)b, (double *)d, (size_t)n);
    }
  }
  return rc;
}

JNIEXPORT jint JNICALL Java_tessera_spark_TesseraNative_nextMany(JNIEnv *env, jclass, jlong h, jlongArray x0, jlongArray x1,
                                                                 jlongArray cnt, jdoubleArray v) {
  const jsize cap = env->GetArrayLength(x0);
  size_t n = 0;
  {
    Pinned p(env);
    jlong *a = (jlong *)p.pin(x0, 0), *b = (jlong *)p.pin(x1, 0), *c = (jlong *)p.pin(cnt, 0);
    jdouble *d = (jdouble *)p.pin(v, 0);
    if (p.ok()) {
      n = tessera_c_next_many((tessera_query_c *)h, (int64_t *)a, (int64_t *)b, (int64_t *)c, (double *)d, (size_t)cap);
    }
  }
  return (jint)n;
}

JNIEXPORT void JNICALL Java_tessera_spark_TesseraNative_setManyInt(JNIEnv *, jclass, jlong h, jint m, jboolean on) {
  tessera_c_set_many_int((tessera_query_c *)h, (size_t)m, on ? 1 : 0);
}

JNIEXPORT jint JNICALL Java_tessera_spark_TesseraNative_manyOverflow(JNIEnv *, jclass, jlong h, jint m, jint product_bits,
                                                                     jint sum_bits) {
  return tessera_c_many_overflow((const tessera_query_c *)h, (size_t)m, product_bits, sum_bits);
}

JNIEXPORT jint JNICALL Java_tessera_spark_TesseraNative_addMany2(JNIEnv *env, jclass, jlong h, jboolean second, jlongArray k,
                                                                 jlongArray x, jdoubleArray v, jlongArray iv, jlongArray nulls,
                                                                 jint n, jint m) {
  int rc = -1;
  const long long nm = (long long)n * m;
  if (n < 0 || m <= 0 || !holds(env, k, n, false) || !holds(env, x, n, false) || !holds(env, v, nm, true) ||
      !holds(env, iv, nm, true) || !holds(env, nulls, n, true)) {
    throw_state(env, "tessera: addMany2 arrays shorter than their rows");
    return rc;
  }
  {
    Pinned p(env);
    jlong *a = (jlong *)p.pin(k, 0), *b = (jlong *)p.pin(x, 0);
    jdouble *d = (jdouble *)p.pin(v, 0);
    jlong *i = iv ? (jlong *)p.pin(iv, 0) : nullptr;
    jlong *m = nulls ? (jlong *)p.pin(nulls, 0) : nullptr;
    if (p.ok()) {
      rc = tessera_c_add_many2((tessera_query_c *)h, second ? 1 : 0, (int64_t *)a, (int64_t *)b, (double *)d,
                               (const int64_t *)i, (const uint64_t *)m, (size_t)n);
    }
  }
  return rc;
}

JNIEXPORT jint JNICALL Java_tessera_spark_TesseraNative_nextMany2(JNIEnv *env, jclass, jlong h, jlongArray x0, jlongArray x1,
                                                                  jlongArray cnt, jdoubleArray v, jlongArray isums,
                                                                  jlongArray nulls, jint m) {
  const jsize cap = env->GetArrayLength(x0);
  size_t n = 0;
  if (m <= 0 || !holds(env, x1, cap, false) || !holds(env, cnt, cap, true) || !holds(env, v, (long long)cap * m, true) ||
      !holds(env, isums, 2LL * cap * m, true) || !holds(env, nulls, cap, true)) {
    throw_state(env, "tessera: nextMany2 arrays shorter than their rows");
    return 0;
  }
  {
    Pinned p(env);
    jlong *a = (jlong *)p.pin(x0, 0), *b = (jlong *)p.pin(x1, 0), *c = (jlong *)p.pin(cnt, 0);
    jdouble *d = (jdouble *)p.pin(v, 0);
    jlong *i = isums ? (jlong *)p.pin(isums, 0) : nullptr;
    jlong *m = nulls ? (jlong *)p.pin(nulls, 0) : nullptr;
    if (p.ok()) {
      n = tessera_c_next_many2((tessera_query_c *)h, (int64_t *)a, (int64_t *)b, (int64_t *)c, (double *)d, (int64_t *)i,
                               (uint64_t *)m, (size_t)cap);
    }
  }
  return (jint)n;
}

JNIEXPORT void JNICALL Java_tessera_spark_TesseraNative_cancel(JNIEnv *, jclass, jlong h) {
  tessera_c_cancel((tessera_query_c *)h);
}

JNIEXPORT jint JNICALL Java_tessera_spark_TesseraNative_next(JNIEnv *env, jclass, jlong h, jlongArray x0,
                                                             jlongArray x1, jlongArray vi, jdoubleArray vd) {
  const jsize cap = env->GetArrayLength(x0);
  size_t n = 0;
  {
    Pinned p(env);
    jlong *a = (jlong *)p.pin(x0, 0), *b = (jlong *)p.pin(x1, 0), *c = (jlong *)p.pin(vi, 0);
    jdouble *d = (jdouble *)p.pin(vd, 0);
    if (!p.ok()) {
      n = (size_t)-1;
    } else {
      n = tessera_c_next_batch((tessera_query_c *)h, (int64_t *)a, (int64_t *)b, (int64_t *)c, (double *)d, (size_t)cap);
    }
  }
  if (n == (size_t)-1) {
    throw_oom(env, "tessera: cannot pin the output arrays");
    return 0;
  }
  return (jint)n;
}

JNIEXPORT jint JNICALL Java_tessera_spark_TesseraNative_next2(JNIEnv *env, jclass, jlong h, jlongArray x0,
                                                              jlongArray x1, jlongArray cnt, jlongArray vi,
                                                              jdoubleArray vd) {
  const jsize cap = env->GetArrayLength(x0);
  size_t n = 0;
  {
    Pinned p(env);
    jlong *a = (jlong *)p.pin(x0, 0), *b = (jlong *)p.pin(x1, 0), *c = (jlong *)p.pin(cnt, 0), *l = (jlong *)p.pin(vi, 0);
    jdouble *d = (jdouble *)p.pin(vd, 0);
    if (!p.ok()) {
      n = (size_t)-1;
    } else {
      n = tessera_c_next_batch2((tessera_query_c *)h, (int64_t *)a, (int64_t *)b, (int64_t *)c, nullptr, (uint64_t *)l,
                                (double *)d, (size_t)cap);
    }
  }
  if (n == (size_t)-1) {
    throw_oom(env, "tessera: cannot pin the output arrays");
    return 0;
  }
  return (jint)n;
}

JNIEXPORT jint JNICALL Java_tessera_spark_TesseraNative_next128(JNIEnv *env, jclass, jlong h, jlongArray x0,
                                                                jlongArray x1, jlongArray hi, jlongArray lo) {
  const jsize cap = env->GetArrayLength(x0);
  size_t n = 0;
  {
    Pinned p(env);
    jlong *a = (jlong *)p.pin(x0, 0), *b = (jlong *)p.pin(x1, 0), *u = (jlong *)p.pin(hi, 0), *l = (jlong *)p.pin(lo, 0);
    if (!p.ok()) {
      n = (size_t)-1;
    } else {
      n = tessera_c_next_batch128((tessera_query_c *)h, (int64_t *)a, (int64_t *)b, (int64_t *)u, (uint64_t *)l,
                                  (size_t)cap);
    }
  }
  if (n == (size_t)-1) {
    throw_oom(env, "tessera: cannot pin the output arrays");
    return 0;
  }
  return (jint)n;
}

JNIEXPORT jint JNICALL Java_tessera_spark_TesseraNative_next2Wide(JNIEnv *env, jclass, jlong h, jlongArray x0,
                                                                  jlongArray x1, jlongArray cnt, jlongArray hi,
                                                                  jlongArray lo, jdoubleArray vd) {
  const jsize cap = env->GetArrayLength(x0);
  size_t n = 0;
  {
    Pinned p(env);
    jlong *a = (jlong *)p.pin(x0, 0), *b = (jlong *)p.pin(x1, 0), *c = (jlong *)p.pin(cnt, 0), *u = (jlong *)p.pin(hi, 0);
    jlong *l = (jlong *)p.pin(lo, 0);
    jdouble *d = (jdouble *)p.pin(vd, 0);
    if (!p.ok()) {
      n = (size_t)-1;
    } else {
      n = tessera_c_next_batch2((tessera_query_c *)h, (int64_t *)a, (int64_t *)b, (int64_t *)c, (int64_t *)u,
                                (uint64_t *)l, (double *)d, (size_t)cap);
    }
  }
  if (n == (size_t)-1) {
    throw_oom(env, "tessera: cannot pin the output arrays");
    return 0;
  }
  return (jint)n;
}

JNIEXPORT void JNICALL Java_tessera_spark_TesseraNative_setTwo(JNIEnv *, jclass, jlong h, jboolean on) {
  tessera_c_set_two((tessera_query_c *)h, on ? 1 : 0);
}

JNIEXPORT jboolean JNICALL Java_tessera_spark_TesseraNative_general(JNIEnv *, jclass, jlong h) {
  return tessera_c_general((const tessera_query_c *)h) ? JNI_TRUE : JNI_FALSE;
}

struct PairFilterRef {
  JavaVM *vm;
  jobject obj;
  jmethodID mid;
};
static std::mutex pair_mu;
static std::map<jlong, PairFilterRef *> pair_refs;

static int pair_pass_jni(void *ctx, int64_t a, int64_t b) {
  PairFilterRef *p = static_cast<PairFilterRef *>(ctx);
  JNIEnv *env = nullptr;
  if (p->vm->GetEnv((void **)&env, JNI_VERSION_1_8) != JNI_OK) return 1;
  const jboolean r = env->CallBooleanMethod(p->obj, p->mid, (jlong)a, (jlong)b);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return 1;
  }
  return r ? 1 : 0;
}

JNIEXPORT void JNICALL Java_tessera_spark_TesseraNative_setPairFilter(JNIEnv *env, jclass, jlong h, jobject filter,
                                                                      jint product_bits) {
  jclass c = env->GetObjectClass(filter);
  jmethodID mid = c ? env->GetMethodID(c, "pass", "(JJ)Z") : nullptr;
  PairFilterRef *p = mid ? new (std::nothrow) PairFilterRef() : nullptr;
  if (!p || env->GetJavaVM(&p->vm) != JNI_OK || !(p->obj = env->NewGlobalRef(filter))) {
    delete p;
    if (!env->ExceptionCheck()) throw_oom(env, "tessera: cannot keep the pair filter");
    return;
  }
  p->mid = mid;
  {
    std::lock_guard<std::mutex> g(pair_mu);
    PairFilterRef *&slot = pair_refs[h];
    if (slot) {
      env->DeleteGlobalRef(slot->obj);
      delete slot;
    }
    slot = p;
  }
  tessera_c_set_pair_filter((tessera_query_c *)h, pair_pass_jni, p, product_bits);
}

JNIEXPORT void JNICALL Java_tessera_spark_TesseraNative_free(JNIEnv *env, jclass, jlong h) {
  tessera_c_free((tessera_query_c *)h);
  std::lock_guard<std::mutex> g(pair_mu);
  auto it = pair_refs.find(h);
  if (it != pair_refs.end()) {
    env->DeleteGlobalRef(it->second->obj);
    delete it->second;
    pair_refs.erase(it);
  }
}

}
