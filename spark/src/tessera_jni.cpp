#include <jni.h>

#include <string>
#include <vector>

#include "tessera_c.h"

extern "C" {

JNIEXPORT jint JNICALL Java_tessera_spark_TesseraNative_apiVersion(JNIEnv *, jclass) { return tessera_c_api_version(); }

JNIEXPORT jstring JNICALL Java_tessera_spark_TesseraNative_version(JNIEnv *env, jclass) {
  std::string v = std::string(tessera_c_version()) + " (" + tessera_c_isa() + ")";
  return env->NewStringUTF(v.c_str());
}

JNIEXPORT jlong JNICALL Java_tessera_spark_TesseraNative_create(JNIEnv *, jclass, jboolean has_value) {
  return (jlong)tessera_c_new(has_value ? 1 : 0);
}

static jint add_rows(JNIEnv *env, jlong h, jlongArray k, jlongArray x, jdoubleArray v, jint n, bool second) {
  jlong *kp = (jlong *)env->GetPrimitiveArrayCritical(k, nullptr);
  jlong *xp = (jlong *)env->GetPrimitiveArrayCritical(x, nullptr);
  jdouble *vp = v ? (jdouble *)env->GetPrimitiveArrayCritical(v, nullptr) : nullptr;
  tessera_query_c *q = (tessera_query_c *)h;
  int rc = second ? tessera_c_add_y(q, (const int64_t *)kp, (const int64_t *)xp, (const double *)vp, (size_t)n)
                  : tessera_c_add(q, (const int64_t *)kp, (const int64_t *)xp, (const double *)vp, (size_t)n);
  if (vp) env->ReleasePrimitiveArrayCritical(v, vp, JNI_ABORT);
  env->ReleasePrimitiveArrayCritical(x, xp, JNI_ABORT);
  env->ReleasePrimitiveArrayCritical(k, kp, JNI_ABORT);
  return rc;
}

JNIEXPORT jint JNICALL Java_tessera_spark_TesseraNative_add(JNIEnv *env, jclass, jlong h, jlongArray k, jlongArray x,
                                                            jdoubleArray v, jint n) {
  return add_rows(env, h, k, x, v, n, false);
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
  jbyte *b = (jbyte *)env->GetPrimitiveArrayCritical(bytes, nullptr);
  jint *o = (jint *)env->GetPrimitiveArrayCritical(offs, nullptr);
  jlong *c = (jlong *)env->GetPrimitiveArrayCritical(out, nullptr);
  int rc = 0;
  for (jint i = 0; i < n && rc == 0; i++) {
    c[i] = tessera_dict_code((tessera_dict_c *)d, (const char *)b + o[i], (size_t)(o[i + 1] - o[i]));
    if (c[i] < 0) rc = -1;
  }
  env->ReleasePrimitiveArrayCritical(out, c, 0);
  env->ReleasePrimitiveArrayCritical(offs, o, JNI_ABORT);
  env->ReleasePrimitiveArrayCritical(bytes, b, JNI_ABORT);
  return rc;
}

JNIEXPORT void JNICALL Java_tessera_spark_TesseraNative_dictFree(JNIEnv *, jclass, jlong d) {
  tessera_dict_free((tessera_dict_c *)d);
}

JNIEXPORT void JNICALL Java_tessera_spark_TesseraNative_setXDict(JNIEnv *, jclass, jlong h, jlong d) {
  tessera_c_set_xdict((tessera_query_c *)h, (tessera_dict_c *)d);
}

typedef size_t (*strings_fn)(const tessera_query_c *, char *, size_t, int64_t *, size_t, size_t *);

static jbyteArray string_bytes(JNIEnv *env, jlong h, strings_fn f) {
  size_t total = 0;
  const size_t d = f((const tessera_query_c *)h, nullptr, 0, nullptr, 0, &total);
  std::vector<char> b(total);
  std::vector<int64_t> o(d + 1);
  f((const tessera_query_c *)h, b.data(), total, o.data(), d + 1, &total);
  jbyteArray r = env->NewByteArray((jsize)total);
  if (r) env->SetByteArrayRegion(r, 0, (jsize)total, (const jbyte *)b.data());
  return r;
}

static jlongArray string_offsets(JNIEnv *env, jlong h, strings_fn f) {
  size_t total = 0;
  const size_t d = f((const tessera_query_c *)h, nullptr, 0, nullptr, 0, &total);
  std::vector<char> b(total);
  std::vector<int64_t> o(d + 1);
  f((const tessera_query_c *)h, b.data(), total, o.data(), d + 1, &total);
  jlongArray r = env->NewLongArray((jsize)(d + 1));
  if (r) env->SetLongArrayRegion(r, 0, (jsize)(d + 1), (const jlong *)o.data());
  return r;
}

JNIEXPORT jbyteArray JNICALL Java_tessera_spark_TesseraNative_xStringBytes(JNIEnv *env, jclass, jlong h) {
  return string_bytes(env, h, tessera_c_xstrings);
}

JNIEXPORT jlongArray JNICALL Java_tessera_spark_TesseraNative_xStringOffsets(JNIEnv *env, jclass, jlong h) {
  return string_offsets(env, h, tessera_c_xstrings);
}

JNIEXPORT void JNICALL Java_tessera_spark_TesseraNative_setYDict(JNIEnv *, jclass, jlong h, jlong d) {
  tessera_c_set_ydict((tessera_query_c *)h, (tessera_dict_c *)d);
}

JNIEXPORT jbyteArray JNICALL Java_tessera_spark_TesseraNative_yStringBytes(JNIEnv *env, jclass, jlong h) {
  return string_bytes(env, h, tessera_c_ystrings);
}

JNIEXPORT jlongArray JNICALL Java_tessera_spark_TesseraNative_yStringOffsets(JNIEnv *env, jclass, jlong h) {
  return string_offsets(env, h, tessera_c_ystrings);
}

JNIEXPORT jstring JNICALL Java_tessera_spark_TesseraNative_run(JNIEnv *env, jclass, jlong h, jint mode,
                                                               jboolean col0_smaller, jlong min_value, jlong topk,
                                                               jint threads, jlong frontier, jboolean verbose) {
  char err[256];
  int rc = tessera_c_run((tessera_query_c *)h, mode, col0_smaller ? 1 : 0, (uint64_t)min_value, topk, threads, frontier,
                         verbose ? 1 : 0, err, sizeof err);
  return rc == TESSERA_C_OK ? nullptr : env->NewStringUTF(err);
}

JNIEXPORT void JNICALL Java_tessera_spark_TesseraNative_setSimilarity(JNIEnv *, jclass, jlong h, jint measure,
                                                                      jdouble min_sim) {
  tessera_c_set_similarity((tessera_query_c *)h, measure, min_sim);
}

JNIEXPORT void JNICALL Java_tessera_spark_TesseraNative_cancel(JNIEnv *, jclass, jlong h) {
  tessera_c_cancel((tessera_query_c *)h);
}

JNIEXPORT jint JNICALL Java_tessera_spark_TesseraNative_next(JNIEnv *env, jclass, jlong h, jlongArray x0,
                                                             jlongArray x1, jlongArray vi, jdoubleArray vd) {
  const jsize cap = env->GetArrayLength(x0);
  jlong *a = (jlong *)env->GetPrimitiveArrayCritical(x0, nullptr);
  jlong *b = (jlong *)env->GetPrimitiveArrayCritical(x1, nullptr);
  jlong *c = (jlong *)env->GetPrimitiveArrayCritical(vi, nullptr);
  jdouble *d = (jdouble *)env->GetPrimitiveArrayCritical(vd, nullptr);
  const size_t n = tessera_c_next_batch((tessera_query_c *)h, (int64_t *)a, (int64_t *)b, (int64_t *)c, (double *)d,
                                        (size_t)cap);
  env->ReleasePrimitiveArrayCritical(vd, d, 0);
  env->ReleasePrimitiveArrayCritical(vi, c, 0);
  env->ReleasePrimitiveArrayCritical(x1, b, 0);
  env->ReleasePrimitiveArrayCritical(x0, a, 0);
  return (jint)n;
}

JNIEXPORT jboolean JNICALL Java_tessera_spark_TesseraNative_general(JNIEnv *, jclass, jlong h) {
  return tessera_c_general((const tessera_query_c *)h) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_tessera_spark_TesseraNative_free(JNIEnv *, jclass, jlong h) {
  tessera_c_free((tessera_query_c *)h);
}

}
