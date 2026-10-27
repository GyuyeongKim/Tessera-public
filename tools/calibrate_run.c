#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tessera_c.h"

static void usage(void) {
  fprintf(stderr,
          "usage: calibrate_run [-t threads] [-r reps] [-k key column] [-x value column] [-m output bytes -d spill dir]\n"
          "                     <file> <frontier>...\n"
          "Counts the pairs of values that share a key in <file> (the self-join grouped by the value column), once\n"
          "untimed and then reps times for every frontier, -1 being the cost model's choice. The library's report of\n"
          "each run goes to stderr.\n");
  exit(2);
}

static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static int parse_int(const char *s, int64_t *out) {
  char *end;
  errno = 0;
  long long v = strtoll(s, &end, 10);
  if (end == s || *end || errno) return 0;
  *out = v;
  return 1;
}

static int fields(char *line, int kc, int xc, int64_t *k, int64_t *x) {
  char *save = NULL;
  int got = 0, c = 0;
  for (char *tok = strtok_r(line, ", \t\r\n", &save); tok; tok = strtok_r(NULL, ", \t\r\n", &save), c++) {
    if (c == kc) got += parse_int(tok, k);
    if (c == xc) got += parse_int(tok, x);
  }
  return got == 2;
}

int main(int argc, char **argv) {
  int threads = 0, reps = 3, kc = 0, xc = 1, o;
  size_t mem = 0;
  const char *dir = NULL;
  while ((o = getopt(argc, argv, "+t:r:k:x:m:d:")) != -1) {
    switch (o) {
      case 't': threads = atoi(optarg); break;
      case 'r': reps = atoi(optarg); break;
      case 'k': kc = atoi(optarg); break;
      case 'x': xc = atoi(optarg); break;
      case 'm': mem = (size_t)strtoull(optarg, NULL, 10); break;
      case 'd': dir = optarg; break;
      default: usage();
    }
  }
  if (argc - optind < 2 || reps < 1 || kc < 0 || xc < 0 || kc == xc) usage();
  FILE *f = fopen(argv[optind], "r");
  if (!f) {
    fprintf(stderr, "calibrate: cannot open %s: %s\n", argv[optind], strerror(errno));
    return 1;
  }
  size_t n = 0, cap = 1 << 20, skipped = 0;
  int64_t *K = malloc(cap * sizeof *K), *X = malloc(cap * sizeof *X);
  char *line = NULL;
  size_t len = 0;
  double t0 = now();
  while (K && X && getline(&line, &len, f) != -1) {
    int64_t k, x;
    if (!fields(line, kc, xc, &k, &x)) {
      skipped++;
      continue;
    }
    if (n == cap) {
      cap *= 2;
      int64_t *k2 = realloc(K, cap * sizeof *K), *x2 = realloc(X, cap * sizeof *X);
      if (!k2 || !x2) {
        K = k2 ? k2 : K;
        X = x2 ? x2 : X;
        fprintf(stderr, "calibrate: out of memory after %zu rows\n", n);
        return 1;
      }
      K = k2;
      X = x2;
    }
    K[n] = k;
    X[n] = x;
    n++;
  }
  free(line);
  fclose(f);
  if (!K || !X || n == 0) {
    fprintf(stderr, "calibrate: no rows with integers in columns %d and %d of %s\n", kc, xc, argv[optind]);
    return 1;
  }
  fprintf(stderr, "calibrate: %zu rows (%zu lines skipped) loaded in %.1f s, library %s, variant %s\n", n, skipped,
          now() - t0, tessera_c_version(), tessera_c_isa());
  char err[512];
  for (int a = optind + 1; a < argc; a++) {
    int64_t frontier;
    if (!parse_int(argv[a], &frontier) || frontier < -1) usage();
    for (int rep = 0; rep <= reps; rep++) {
      tessera_query_c *q = tessera_c_new(0);
      if (!q || tessera_c_add(q, K, X, NULL, n) != TESSERA_C_OK) {
        fprintf(stderr, "calibrate: cannot build the query\n");
        return 1;
      }
      if (mem) tessera_c_set_memory(q, mem, dir);
      fprintf(stderr, "calibrate: frontier %lld rep %d\n", (long long)frontier, rep);
      double t = now();
      int rc = tessera_c_run(q, 2, 1, 0, -1, threads, frontier, 1, err, sizeof err);
      t = now() - t;
      if (rc != TESSERA_C_OK) {
        fprintf(stderr, "calibrate: run failed: %s\n", err);
        return 1;
      }
      fprintf(stderr, "calibrate: done frontier %lld rep %d seconds %.4f\n", (long long)frontier, rep, t);
      fflush(stderr);
      tessera_c_free(q);
    }
  }
  free(K);
  free(X);
  return 0;
}
