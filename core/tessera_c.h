#ifndef TESSERA_C_H
#define TESSERA_C_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TESSERA_API_VERSION 2
int tessera_c_api_version(void);
const char *tessera_c_version(void);
const char *tessera_c_isa(void);

#define TESSERA_C_OK 0
#define TESSERA_C_ERROR (-1)
#define TESSERA_C_CANCELLED (-2)

typedef struct tessera_query_c tessera_query_c;
typedef struct tessera_dict_c tessera_dict_c;

tessera_query_c *tessera_c_new(int has_value);
tessera_query_c *tessera_c_new_cross(int has_value);
int tessera_c_add_y(tessera_query_c *q, const int64_t *k, const int64_t *y, const double *w, size_t n);
int tessera_c_add(tessera_query_c *q, const int64_t *k, const int64_t *x, const double *v, size_t n);

tessera_dict_c *tessera_dict_new(void);
int64_t tessera_dict_code(tessera_dict_c *d, const char *s, size_t len);
void tessera_dict_free(tessera_dict_c *d);
void tessera_c_set_xdict(tessera_query_c *q, tessera_dict_c *d);
const char *tessera_c_xstr(const tessera_query_c *q, int64_t x, size_t *len);
void tessera_c_set_ydict(tessera_query_c *q, tessera_dict_c *d);
const char *tessera_c_ystr(const tessera_query_c *q, int64_t y, size_t *len);

void tessera_c_set_memory(tessera_query_c *q, size_t out_budget, const char *spill_dir);
void tessera_c_set_stop(tessera_query_c *q, int (*should_stop)(void *), void *ctx);
void tessera_c_cancel(tessera_query_c *q);
size_t tessera_c_xstrings(const tessera_query_c *q, char *bytes, size_t bytes_cap, int64_t *offsets, size_t d_cap,
                          size_t *bytes_out);
size_t tessera_c_ystrings(const tessera_query_c *q, char *bytes, size_t bytes_cap, int64_t *offsets, size_t d_cap,
                          size_t *bytes_out);
void tessera_c_set_keep_threads(tessera_query_c *q, int keep);
void tessera_c_set_similarity(tessera_query_c *q, int measure, double min_sim);

int tessera_c_run(tessera_query_c *q, int mode, int col0_smaller, uint64_t min_value, int64_t topk, int threads,
                  int64_t frontier, int verbose, char *err, size_t errlen);
int tessera_c_next(tessera_query_c *q, int64_t *x0, int64_t *x1, int64_t *vi, double *vd, int *general);
size_t tessera_c_next_batch(tessera_query_c *q, int64_t *x0, int64_t *x1, int64_t *vi, double *vd, size_t cap);
int tessera_c_general(const tessera_query_c *q);
void tessera_c_rewind(tessera_query_c *q);
void tessera_c_free(tessera_query_c *q);

#ifdef __cplusplus
}
#endif
#endif
