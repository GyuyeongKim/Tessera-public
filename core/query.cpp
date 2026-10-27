#include "query.hpp"

#include <dlfcn.h>
#include <omp.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace tessera {

using std::vector;

static double Now() {
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static std::string Fmt(const char *f, ...) {
	char buf[512];
	va_list ap;
	va_start(ap, f);
	vsnprintf(buf, sizeof buf, f, ap);
	va_end(ap);
	return buf;
}

int64_t StrDict::code(const char *s, size_t len) {
	auto it = map_.emplace(std::string(s, len), (int64_t)strs_.size());
	if (it.second) {
		strs_.push_back(&it.first->first);
	}
	return it.first->second;
}

std::vector<int64_t> StrDict::finish() {
	const size_t n = strs_.size();
	vector<int64_t> order(n), old2new(n);
	for (size_t c = 0; c < n; c++) {
		order[c] = (int64_t)c;
	}
	std::sort(order.begin(), order.end(), [&](int64_t a, int64_t b) { return *strs_[a] < *strs_[b]; });
	vector<const std::string *> sorted(n);
	for (size_t c = 0; c < n; c++) {
		old2new[order[c]] = (int64_t)c;
		sorted[c] = strs_[order[c]];
		map_[*sorted[c]] = (int64_t)c;
	}
	strs_.swap(sorted);
	return old2new;
}

double cell_value(uint64_t s, bool general) {
	if (!general) {
		return (double)(int64_t)s;
	}
	double d;
	memcpy(&d, &s, sizeof d);
	return d;
}

int overflow_flags(const QueryResult &r, int product_bits, int sum_bits) {
	if (!r.int_values) {
		return 0;
	}
	auto outside = [](__int128 lo, __int128 hi, int bits) {
		if (bits <= 0 || bits >= 128) {
			return false;
		}
		const __int128 top = ((__int128)1 << (bits - 1)) - 1;
		return lo < -top - 1 || hi > top;
	};
	return (outside(r.prod_min, r.prod_max, product_bits) ? 1 : 0) | (outside(r.cell_min, r.cell_max, sum_bits) ? 2 : 0);
}

struct OomFlag {
	std::atomic<bool> set {false};
	template <class F>
	bool run(F &&f) {
		try {
			f();
			return true;
		} catch (const std::bad_alloc &) {
			set.store(true, std::memory_order_relaxed);
			return false;
		}
	}
	void check(const char *what) const {
		if (set.load()) {
			throw OutOfMemory(std::string("tessera: out of memory (") + what + ")");
		}
	}
};

template <class V>
static void ParSort(vector<V> &a, int T) {
	const size_t n = a.size();
	if (T <= 1 || n < (size_t)T * 4096) {
		std::sort(a.begin(), a.end());
		return;
	}
	vector<V> b(n), samp((size_t)T * T), spl((size_t)T - 1);
	vector<size_t> lo((size_t)T + 1), cut((size_t)T * (T + 1)), off((size_t)T + 1, 0);
	for (int t = 0; t <= T; t++) {
		lo[t] = n * (size_t)t / (size_t)T;
	}
#pragma omp parallel for num_threads(T) schedule(static, 1)
	for (int t = 0; t < T; t++) {
		std::sort(a.begin() + lo[t], a.begin() + lo[t + 1]);
		for (int s = 0; s < T; s++) {
			samp[(size_t)t * T + s] = a[lo[t] + (lo[t + 1] - lo[t]) * (size_t)s / (size_t)T];
		}
	}
	std::sort(samp.begin(), samp.end());
	for (int s = 1; s < T; s++) {
		spl[s - 1] = samp[(size_t)s * T];
	}
#pragma omp parallel for num_threads(T) schedule(static, 1)
	for (int t = 0; t < T; t++) {
		size_t *c = &cut[(size_t)t * (T + 1)];
		c[0] = lo[t];
		for (int u = 1; u < T; u++) {
			c[u] = std::lower_bound(a.begin() + c[u - 1], a.begin() + lo[t + 1], spl[u - 1]) - a.begin();
		}
		c[T] = lo[t + 1];
	}
	for (int u = 0; u < T; u++) {
		size_t m = 0;
		for (int t = 0; t < T; t++) {
			m += cut[(size_t)t * (T + 1) + u + 1] - cut[(size_t)t * (T + 1) + u];
		}
		off[u + 1] = off[u] + m;
	}
#pragma omp parallel for num_threads(T) schedule(dynamic, 1)
	for (int u = 0; u < T; u++) {
		size_t o = off[u];
		for (int t = 0; t < T; t++) {
			const size_t *c = &cut[(size_t)t * (T + 1)];
			o = std::copy(a.begin() + c[u], a.begin() + c[u + 1], b.begin() + o) - b.begin();
		}
		std::sort(b.begin() + off[u], b.begin() + off[u + 1]);
	}
	a.swap(b);
}

static const int64_t POLL_EVERY = 1 << 16;

static const double RANGE_PER_DISTINCT = 4;
static const int32_t TOPK_FIRST = 256;

static int64_t CountDistinct(std::vector<std::unique_ptr<QueryPart>> &parts, vector<int64_t> QueryPart::*col, int64_t mn,
                             uint64_t span, int T, const std::function<bool(int64_t)> &poll) {
	vector<std::atomic<uint64_t>> bits(span / 64 + 1);
#pragma omp parallel num_threads(T)
	{
#pragma omp for schedule(static)
		for (int64_t w = 0; w < (int64_t)bits.size(); w++) {
			bits[w].store(0, std::memory_order_relaxed);
		}
		bool off = false;
		for (auto &p : parts) {
			const auto &c = (*p).*col;
#pragma omp for schedule(static) nowait
			for (int64_t e = 0; e < (int64_t)c.size(); e++) {
				if (off || ((e & (POLL_EVERY - 1)) == 0 && (off = poll(e)))) {
					continue;
				}
				const uint64_t o = (uint64_t)c[e] - (uint64_t)mn;
				const uint64_t b = 1ull << (o & 63);
				if (!(bits[o >> 6].load(std::memory_order_relaxed) & b)) {
					bits[o >> 6].fetch_or(b, std::memory_order_relaxed);
				}
			}
		}
	}
	int64_t d = 0;
#pragma omp parallel for num_threads(T) schedule(static) reduction(+ : d)
	for (int64_t w = 0; w < (int64_t)bits.size(); w++) {
		d += __builtin_popcountll(bits[w].load(std::memory_order_relaxed));
	}
	return d;
}

struct I64Table {
	vector<int64_t> key;
	vector<int32_t> val;
	uint64_t mask = 0;
	size_t n = 0;
	explicit I64Table(size_t cap) : key(cap), val(cap, -1), mask(cap - 1) {
	}
	static uint64_t Hash(int64_t v) {
		uint64_t z = (uint64_t)v * 0x9E3779B97F4A7C15ull;
		return z ^ (z >> 32);
	}
	void Insert(int64_t v) {
		uint64_t h = Hash(v) & mask;
		while (val[h] >= 0 && key[h] != v) {
			h = (h + 1) & mask;
		}
		if (val[h] < 0) {
			key[h] = v;
			val[h] = 0;
			if (++n * 2 > key.size()) {
				Grow();
			}
		}
	}
	void Grow() {
		I64Table t(key.size() * 2);
		for (size_t s = 0; s < key.size(); s++) {
			if (val[s] >= 0) {
				t.Insert(key[s]);
			}
		}
		std::swap(*this, t);
	}
	void InsertDistinct(int64_t v, int32_t c) {
		uint64_t h = Hash(v) & mask;
		int32_t empty = -1;
		while (!__atomic_compare_exchange_n(&val[h], &empty, c, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
			empty = -1;
			h = (h + 1) & mask;
		}
		key[h] = v;
	}
	int32_t Get(int64_t v) const {
		uint64_t h = Hash(v) & mask;
		while (key[h] != v) {
			h = (h + 1) & mask;
		}
		return val[h];
	}
};

static vector<int64_t> Encode(std::vector<std::unique_ptr<QueryPart>> &parts, vector<int64_t> QueryPart::*col, int T) {
	vector<vector<int64_t>> found(T);
	OomFlag oom;
#pragma omp parallel num_threads(T)
	{
		std::unique_ptr<I64Table> set;
		bool ok = oom.run([&] { set.reset(new I64Table(1 << 16)); });
		for (auto &p : parts) {
			const auto &c = (*p).*col;
#pragma omp for schedule(static) nowait
			for (int64_t e = 0; e < (int64_t)c.size(); e++) {
				if (ok) {
					ok = oom.run([&] { set->Insert(c[e]); });
				}
			}
		}
		if (ok) {
			auto &f = found[omp_get_thread_num()];
			oom.run([&] {
				f.reserve(set->n);
				for (size_t s = 0; s < set->key.size(); s++) {
					if (set->val[s] >= 0) {
						f.push_back(set->key[s]);
					}
				}
			});
		}
	}
	oom.check("key dictionary");
	vector<int64_t> d;
	for (auto &f : found) {
		d.insert(d.end(), f.begin(), f.end());
		vector<int64_t>().swap(f);
	}
	ParSort(d, T);
	d.erase(std::unique(d.begin(), d.end()), d.end());
	d.shrink_to_fit();
	if (d.size() > (size_t)INT32_MAX) {
		throw std::runtime_error("tessera: more than 2^31 - 1 distinct join keys or group keys");
	}
	size_t cap = 2;
	while (cap < 2 * d.size()) {
		cap *= 2;
	}
	I64Table map(cap);
#pragma omp parallel for num_threads(T) schedule(static)
	for (int64_t i = 0; i < (int64_t)d.size(); i++) {
		map.InsertDistinct(d[i], (int32_t)i);
	}
	for (auto &p : parts) {
		auto &c = (*p).*col;
#pragma omp parallel for num_threads(T) schedule(static)
		for (int64_t e = 0; e < (int64_t)c.size(); e++) {
			c[e] = map.Get(c[e]);
		}
	}
	return d;
}

struct SideCodes {
	int64_t count = 0, min = 0;
	vector<int64_t> dict;
	int64_t value(int64_t code) const {
		return dict.empty() ? min + code : dict[(size_t)code];
	}
};

static SideCodes EncodeSide(std::vector<std::unique_ptr<QueryPart>> &parts, int T,
                            const std::function<bool(int64_t)> &poll) {
	SideCodes sc;
	int64_t mn = INT64_MAX, mx = INT64_MIN;
	size_t n = 0;
	for (auto &p : parts) {
		n += p->x.size();
		for (auto x : p->x) {
			mn = std::min(mn, x);
			mx = std::max(mx, x);
		}
	}
	if (n == 0) {
		return sc;
	}
	const uint64_t span = (uint64_t)mx - (uint64_t)mn;
	const double range = (double)span + 1;
	if (span < (1ull << 28) && range <= RANGE_PER_DISTINCT * (double)n &&
	    range <= RANGE_PER_DISTINCT * (double)CountDistinct(parts, &QueryPart::x, mn, span, T, poll)) {
		sc.min = mn;
		sc.count = (int64_t)span + 1;
		for (auto &p : parts) {
#pragma omp parallel for num_threads(T) schedule(static)
			for (int64_t e = 0; e < (int64_t)p->x.size(); e++) {
				p->x[e] -= mn;
			}
		}
	} else {
		sc.dict = Encode(parts, &QueryPart::x, T);
		sc.count = (int64_t)sc.dict.size();
	}
	return sc;
}

static Csr PermuteItems(const Csr &X, const vector<int32_t> &lay, int T) {
	Csr Z;
	Z.n_rows = X.n_rows;
	Z.n_cols = X.n_cols;
	Z.ptr = X.ptr;
	Z.idx.resize(X.idx.size());
	Z.val.resize(X.val.size());
	Z.ival.resize(X.ival.size());
	Z.wval.resize(X.wval.size());
	Z.xval.resize(X.xval.size());
	Z.xval_wide.resize(X.xval_wide.size());
	int64_t maxlen = 0;
	for (int64_t r = 0; r < X.n_rows; r++) {
		maxlen = std::max(maxlen, X.ptr[r + 1] - X.ptr[r]);
	}
	OomFlag oom;
#pragma omp parallel num_threads(T)
	{
		vector<std::pair<int32_t, int64_t>> tmp;
		const bool ok = oom.run([&] { tmp.reserve(maxlen); });
#pragma omp for schedule(dynamic, 1024)
		for (int64_t r = 0; r < X.n_rows; r++) {
			if (!ok) {
				continue;
			}
			tmp.clear();
			for (int64_t e = X.ptr[r]; e < X.ptr[r + 1]; e++) {
				tmp.push_back({lay[X.idx[e]], e});
			}
			std::sort(tmp.begin(), tmp.end());
			for (size_t q = 0; q < tmp.size(); q++) {
				const int64_t o = X.ptr[r] + (int64_t)q;
				Z.idx[o] = tmp[q].first;
				if (!X.val.empty()) {
					Z.val[o] = X.val[tmp[q].second];
				}
				if (!X.ival.empty()) {
					Z.ival[o] = X.ival[tmp[q].second];
				}
				if (!X.wval.empty()) {
					Z.wval[o] = X.wval[tmp[q].second];
				}
				if (!X.xval.empty()) {
					Z.xval[o] = X.xval[tmp[q].second];
				}
				if (!X.xval_wide.empty()) {
					Z.xval_wide[o] = X.xval_wide[tmp[q].second];
				}
			}
		}
	}
	oom.check("item layout");
	return Z;
}

static std::atomic<int> active_teams {0};
static bool pause_frees_own_pool() {
	static const bool gomp = [] {
		Dl_info d;
		return dladdr((void *)&omp_pause_resource_all, &d) != 0 && d.dli_fname != nullptr &&
		       strstr(d.dli_fname, "libgomp") != nullptr;
	}();
	return gomp;
}
struct Team {
	int saved;
	bool release;
	bool pinned = false;
#ifdef __linux__
	cpu_set_t caller_mask;
#endif
	Team(int T, bool rel) : saved(omp_get_max_threads()), release(rel) {
		vector<int> cpus;
#ifdef __linux__
		cpus.reserve(CPU_SETSIZE);
#endif
		const bool alone = active_teams.fetch_add(1) == 0;
#ifdef __linux__
		const bool env_bind = omp_get_proc_bind() != omp_proc_bind_false || getenv("GOMP_CPU_AFFINITY") != nullptr;
		if (alone && !env_bind && sched_getaffinity(0, sizeof caller_mask, &caller_mask) == 0) {
			const int me = sched_getcpu();
			const int from = me >= 0 && me < CPU_SETSIZE ? me : 0;
			for (int i = 0; i < CPU_SETSIZE; i++) {
				const int c = (from + i) % CPU_SETSIZE;
				if (CPU_ISSET(c, &caller_mask)) {
					cpus.push_back(c);
				}
			}
			pinned = cpus.size() > 1;
		}
#else
		(void)alone;
#endif
		sigset_t all, old;
		sigfillset(&all);
		pthread_sigmask(SIG_BLOCK, &all, &old);
#pragma omp parallel num_threads(T)
		{
#ifdef __linux__
			if (pinned) {
				cpu_set_t one;
				CPU_ZERO(&one);
				CPU_SET(cpus[omp_get_thread_num() % cpus.size()], &one);
				pthread_setaffinity_np(pthread_self(), sizeof one, &one);
			}
#endif
		}
		pthread_sigmask(SIG_SETMASK, &old, nullptr);
		omp_set_num_threads(T);
	}
	~Team() {
		omp_set_num_threads(saved);
#ifdef __linux__
		if (pinned) {
			pthread_setaffinity_np(pthread_self(), sizeof caller_mask, &caller_mask);
		}
#endif
		if (release && pause_frees_own_pool()) {
			omp_pause_resource_all(omp_pause_soft);
		}
		active_teams.fetch_sub(1);
	}
};

static Csr RestrictItems(const Csr &X, int32_t h, int T, int32_t lo = 0) {
	vector<int64_t> len((size_t)X.n_rows);
#pragma omp parallel for num_threads(T) schedule(static)
	for (int64_t r = 0; r < X.n_rows; r++) {
		const auto b = X.idx.begin() + X.ptr[r];
		const int64_t n = std::lower_bound(b, X.idx.begin() + X.ptr[r + 1], h) - b;
		len[r] = lo > 0 && (n == 0 || b[n - 1] < lo) ? -1 : n;
	}
	vector<int32_t> src;
	if (lo > 0) {
		for (int64_t r = 0; r < X.n_rows; r++) {
			if (len[r] >= 0) {
				src.push_back((int32_t)r);
			}
		}
	}
	Csr Y;
	Y.n_rows = lo > 0 ? (int32_t)src.size() : X.n_rows;
	Y.n_cols = std::min(X.n_cols, h);
	auto row = [&](int64_t r) { return lo > 0 ? (int64_t)src[r] : r; };
	Y.ptr.assign((size_t)Y.n_rows + 1, 0);
	for (int64_t r = 0; r < Y.n_rows; r++) {
		Y.ptr[r + 1] = Y.ptr[r] + len[row(r)];
	}
	Y.idx.resize(Y.ptr[Y.n_rows]);
	if (!X.val.empty()) {
		Y.val.resize(Y.idx.size());
	}
	if (!X.ival.empty()) {
		Y.ival.resize(Y.idx.size());
	}
	if (!X.wval.empty()) {
		Y.wval.resize(Y.idx.size());
	}
	if (!X.xval.empty()) {
		Y.xval.resize(Y.idx.size());
	}
	if (!X.xval_wide.empty()) {
		Y.xval_wide.resize(Y.idx.size());
	}
#pragma omp parallel for num_threads(T) schedule(static)
	for (int64_t r = 0; r < Y.n_rows; r++) {
		const int64_t n = Y.ptr[r + 1] - Y.ptr[r], a = X.ptr[row(r)];
		std::copy(X.idx.begin() + a, X.idx.begin() + a + n, Y.idx.begin() + Y.ptr[r]);
		if (!X.val.empty()) {
			std::copy(X.val.begin() + a, X.val.begin() + a + n, Y.val.begin() + Y.ptr[r]);
		}
		if (!X.ival.empty()) {
			std::copy(X.ival.begin() + a, X.ival.begin() + a + n, Y.ival.begin() + Y.ptr[r]);
		}
		if (!X.wval.empty()) {
			std::copy(X.wval.begin() + a, X.wval.begin() + a + n, Y.wval.begin() + Y.ptr[r]);
		}
		if (!X.xval.empty()) {
			std::copy(X.xval.begin() + a, X.xval.begin() + a + n, Y.xval.begin() + Y.ptr[r]);
		}
		if (!X.xval_wide.empty()) {
			std::copy(X.xval_wide.begin() + a, X.xval_wide.begin() + a + n, Y.xval_wide.begin() + Y.ptr[r]);
		}
	}
	return Y;
}

static void MergeRows(Csr &X, QueryResult &qr, int T, uint64_t RK, bool has_v, const vector<int64_t> &ptr,
                      const vector<int32_t> &idx, const vector<double> &val, const vector<int64_t> &keep, bool dup) {
	const bool need_val = has_v || dup;
	X.ptr.assign(RK + 1, 0);
	for (uint64_t r = 0; r < RK; r++) {
		X.ptr[r + 1] = X.ptr[r] + keep[r + 1];
	}
	X.idx.resize(X.ptr[RK]);
	vector<double> mv(need_val ? X.ptr[RK] : 0);
#pragma omp parallel for num_threads(T) schedule(dynamic, 4096)
	for (int64_t r = 0; r < (int64_t)RK; r++) {
		int64_t w = X.ptr[r] - 1;
		for (int64_t q = ptr[r]; q < ptr[r + 1]; q++) {
			double v = has_v ? val[q] : 1.0;
			if (q > ptr[r] && idx[q] == idx[q - 1]) {
				if (need_val) {
					mv[w] += v;
				}
			} else {
				w++;
				X.idx[w] = idx[q];
				if (need_val) {
					mv[w] = v;
				}
			}
		}
	}
	if (need_val) {
		bool small = true, ints = true, all_one = true;
#pragma omp parallel for num_threads(T) reduction(&& : small, ints, all_one)
		for (int64_t q = 0; q < (int64_t)mv.size(); q++) {
			const bool integer = mv[q] >= 1 && mv[q] <= 65535 && mv[q] == (double)(int64_t)mv[q];
			ints = ints && integer;
			small = small && integer && mv[q] <= 255;
			all_one = all_one && mv[q] == 1;
		}
		if (!ints) {
			X.wval = std::move(mv);
			qr.general = true;
		} else if (!small) {
			X.ival.resize(mv.size());
#pragma omp parallel for num_threads(T) schedule(static)
			for (int64_t q = 0; q < (int64_t)mv.size(); q++) {
				X.ival[q] = (uint32_t)mv[q];
			}
		} else if (!all_one) {
			X.val.resize(mv.size());
#pragma omp parallel for num_threads(T) schedule(static)
			for (int64_t q = 0; q < (int64_t)mv.size(); q++) {
				X.val[q] = (uint8_t)mv[q];
			}
		}
	}
}

static void MergeRowsInt(Csr &X, int T, uint64_t RK, const vector<int64_t> &ptr, const vector<int32_t> &idx,
                         const vector<int64_t> &val, const vector<int64_t> &keep) {
	X.ptr.assign(RK + 1, 0);
	for (uint64_t r = 0; r < RK; r++) {
		X.ptr[r + 1] = X.ptr[r] + keep[r + 1];
	}
	X.idx.resize(X.ptr[RK]);
	vector<int64_t> mv(X.ptr[RK]);
	std::atomic<bool> over {false};
#pragma omp parallel for num_threads(T) schedule(dynamic, 4096)
	for (int64_t r = 0; r < (int64_t)RK; r++) {
		int64_t w = X.ptr[r] - 1;
		for (int64_t q = ptr[r]; q < ptr[r + 1]; q++) {
			if (q > ptr[r] && idx[q] == idx[q - 1]) {
				if (__builtin_add_overflow(mv[w], val[q], &mv[w])) {
					over.store(true, std::memory_order_relaxed);
				}
			} else {
				w++;
				X.idx[w] = idx[q];
				mv[w] = val[q];
			}
		}
	}
	if (over.load()) {
		vector<int64_t>().swap(mv);
		X.xval_wide.resize(X.ptr[RK]);
#pragma omp parallel for num_threads(T) schedule(dynamic, 4096)
		for (int64_t r = 0; r < (int64_t)RK; r++) {
			int64_t w = X.ptr[r] - 1;
			for (int64_t q = ptr[r]; q < ptr[r + 1]; q++) {
				if (q > ptr[r] && idx[q] == idx[q - 1]) {
					X.xval_wide[w] += val[q];
				} else {
					X.xval_wide[++w] = val[q];
				}
			}
		}
		return;
	}
	bool small = true, ints = true, all_one = true;
#pragma omp parallel for num_threads(T) reduction(&& : small, ints, all_one)
	for (int64_t q = 0; q < (int64_t)mv.size(); q++) {
		ints = ints && mv[q] >= 1 && mv[q] <= 65535;
		small = small && mv[q] >= 1 && mv[q] <= 255;
		all_one = all_one && mv[q] == 1;
	}
	if (!ints) {
		X.xval = std::move(mv);
	} else if (!small) {
		X.ival.resize(mv.size());
#pragma omp parallel for num_threads(T) schedule(static)
		for (int64_t q = 0; q < (int64_t)mv.size(); q++) {
			X.ival[q] = (uint32_t)mv[q];
		}
	} else if (!all_one) {
		X.val.resize(mv.size());
#pragma omp parallel for num_threads(T) schedule(static)
		for (int64_t q = 0; q < (int64_t)mv.size(); q++) {
			X.val[q] = (uint8_t)mv[q];
		}
	}
}

struct ProdRange {
	__int128 lo = 0, hi = 0;
	bool any = false;
	void add(__int128 p) {
		if (!any || p < lo) {
			lo = p;
		}
		if (!any || p > hi) {
			hi = p;
		}
		any = true;
	}
	void add(const ProdRange &o) {
		if (o.any) {
			add(o.lo);
			add(o.hi);
		}
	}
};

static void RowProducts(const std::pair<int32_t, int64_t> *t, int64_t m, PairMode mode, const uint8_t *side, ProdRange &pr) {
	if (m == 0) {
		return;
	}
	if (side) {
		int64_t mx[2] = {INT64_MIN, INT64_MIN}, mn[2] = {INT64_MAX, INT64_MAX};
		for (int64_t q = 0; q < m; q++) {
			const int s = side[t[q].first];
			mx[s] = std::max(mx[s], t[q].second);
			mn[s] = std::min(mn[s], t[q].second);
		}
		if (mx[0] >= mn[0] && mx[1] >= mn[1]) {
			for (int64_t a : {mx[0], mn[0]}) {
				for (int64_t b : {mx[1], mn[1]}) {
					pr.add((__int128)a * b);
				}
			}
		}
		return;
	}
	if (mode != PairMode::LT) {
		int64_t mx = INT64_MIN, mn = INT64_MAX;
		for (int64_t q = 0; q < m; q++) {
			mx = std::max(mx, t[q].second);
			mn = std::min(mn, t[q].second);
		}
		pr.add((__int128)mx * mx);
		pr.add((__int128)mn * mn);
		pr.add((__int128)mx * mn);
		return;
	}
	struct E {
		int64_t v;
		int32_t g;
	};
	E top_hi[2], bot_hi[2], top_lo[2], bot_lo[2];
	int nt = 0;
	auto keep_top = [](E *l, int n, E e) {
		if (n < 2) {
			l[n] = e;
			if (n == 1 && l[1].v > l[0].v) {
				std::swap(l[0], l[1]);
			}
		} else if (e.v > l[1].v) {
			l[1] = e;
			if (l[1].v > l[0].v) {
				std::swap(l[0], l[1]);
			}
		}
	};
	auto keep_bot = [](E *l, int n, E e) {
		if (n < 2) {
			l[n] = e;
			if (n == 1 && l[1].v < l[0].v) {
				std::swap(l[0], l[1]);
			}
		} else if (e.v < l[1].v) {
			l[1] = e;
			if (l[1].v < l[0].v) {
				std::swap(l[0], l[1]);
			}
		}
	};
	for (int64_t q = 0; q < m;) {
		const int32_t g = t[q].first;
		int64_t hi = t[q].second, lo = t[q].second;
		for (q++; q < m && t[q].first == g; q++) {
			hi = std::max(hi, t[q].second);
			lo = std::min(lo, t[q].second);
		}
		const int n = std::min(nt, 2);
		keep_top(top_hi, n, {hi, g});
		keep_bot(bot_hi, n, {hi, g});
		keep_top(top_lo, n, {lo, g});
		keep_bot(bot_lo, n, {lo, g});
		nt++;
	}
	if (nt < 2) {
		return;
	}
	E pool[8];
	int np = 0;
	for (E *l : {top_hi, bot_hi, top_lo, bot_lo}) {
		pool[np++] = l[0];
		pool[np++] = l[1];
	}
	for (int a = 0; a < np; a++) {
		for (int b = a + 1; b < np; b++) {
			if (pool[a].g != pool[b].g) {
				pr.add((__int128)pool[a].v * pool[b].v);
			}
		}
	}
}

static void RunQuery(std::vector<std::unique_ptr<QueryPart>> &parts, std::vector<std::unique_ptr<QueryPart>> *parts_y,
                     const QuerySpec &spec, QueryResult &qr) {
	const int T = std::min(spec.threads > 0 ? spec.threads : omp_get_max_threads(), std::max(1, omp_get_num_procs()));
	const bool has_value = spec.has_value;
	const bool verbose = spec.verbose;
	Team team(T, spec.release_threads);
	const bool cross = parts_y != nullptr;
	if (cross && spec.similarity != Similarity::NONE) {
		throw std::runtime_error("tessera: similarity is defined for the self-join only");
	}
	std::atomic<bool> stopped {false};
	auto poll = [&](int64_t i) {
		if ((i & (POLL_EVERY - 1)) != 0) {
			return false;
		}
		if (spec.should_stop && omp_get_thread_num() == 0 && !stopped.load(std::memory_order_relaxed) &&
		    spec.should_stop(spec.stop_ctx)) {
			stopped.store(true, std::memory_order_relaxed);
		}
		return stopped.load(std::memory_order_relaxed);
	};
	auto check = [&]() {
		if (stopped.load() || (spec.should_stop && spec.should_stop(spec.stop_ctx))) {
			throw Cancelled();
		}
	};
	SideCodes cx, cy;
	qr.side.clear();
	if (cross) {
		cx = EncodeSide(parts, T, poll);
		check();
		cy = EncodeSide(*parts_y, T, poll);
		check();
		for (auto &p : *parts_y) {
#pragma omp parallel for num_threads(T) schedule(static)
			for (int64_t e = 0; e < (int64_t)p->x.size(); e++) {
				p->x[e] += cx.count;
			}
			parts.push_back(std::move(p));
		}
		parts_y->clear();
	}
	double t0 = Now();
	const int P = (int)parts.size();
	int64_t minx = INT64_MAX, maxx = INT64_MIN, mink = INT64_MAX, maxk = INT64_MIN;
	size_t n = 0;
	for (auto &p : parts) {
		if (p->x.size() != p->k.size()) {
			throw std::runtime_error("tessera: a part has group keys for some of its rows");
		}
		n += p->k.size();
	}
	bool int_in = false;
	if (has_value) {
		size_t ni = 0, nd = 0;
		for (auto &p : parts) {
			if (p->k.empty()) {
				continue;
			}
			const bool hi = !p->iv.empty();
			if (hi && !p->v.empty()) {
				throw std::runtime_error("tessera: integer and double values in one query");
			}
			if ((hi ? p->iv.size() : p->v.size()) != p->k.size()) {
				throw std::runtime_error("tessera: a part has values for some of its rows");
			}
			(hi ? ni : nd)++;
		}
		if (ni > 0 && nd > 0) {
			throw std::runtime_error("tessera: integer and double values in one query");
		}
		int_in = ni > 0;
	}
	qr.int_values = int_in;
	qr.prod_min = qr.prod_max = qr.cell_min = qr.cell_max = 0;
	qr.wide = false;
	qr.general = false;
	bool ones = true;
#pragma omp parallel for num_threads(T) schedule(dynamic, 1) reduction(min : minx, mink) reduction(max : maxx, maxk) \
    reduction(&& : ones)
	for (int p = 0; p < P; p++) {
		for (auto x : parts[p]->x) {
			minx = std::min(minx, x);
			maxx = std::max(maxx, x);
		}
		for (auto k : parts[p]->k) {
			mink = std::min(mink, k);
			maxk = std::max(maxk, k);
		}
		if (has_value && int_in) {
			for (auto v : parts[p]->iv) {
				ones = ones && v == 1;
			}
		} else if (has_value) {
			for (auto v : parts[p]->v) {
				ones = ones && v == 1;
			}
		}
	}
	const bool has_v = has_value && !ones;
	const bool has_iv = has_v && int_in;
	double tb1 = Now();
	Csr X;
	if (n == 0) {
		qr.out.clear();
		return;
	}
	check();
	auto use_dict = [&](uint64_t span, uint64_t limit, const char *env, vector<int64_t> QueryPart::*col, int64_t mn) {
		const char *f = getenv(env);
		if (span >= limit) {
			return true;
		}
		if (f && strcmp(f, "direct") == 0) {
			return false;
		}
		if (f && strcmp(f, "dictionary") == 0) {
			return true;
		}
		const double range = (double)span + 1;
		if (range > RANGE_PER_DISTINCT * (double)n) {
			return true;
		}
		return range > RANGE_PER_DISTINCT * (double)CountDistinct(parts, col, mn, span, T, poll);
	};
	const uint64_t spanx = (uint64_t)maxx - (uint64_t)minx, spank = (uint64_t)maxk - (uint64_t)mink;
	vector<int64_t> dictx;
	bool dict_k = false;
	if (use_dict(spanx, 1ull << 28, "TESSERA_KEYS_X", &QueryPart::x, minx)) {
		dictx = Encode(parts, &QueryPart::x, T);
		minx = 0;
		maxx = (int64_t)dictx.size() - 1;
	}
	if (use_dict(spank, (1ull << 31) - 1, "TESSERA_KEYS_K", &QueryPart::k, mink)) {
		const size_t d = Encode(parts, &QueryPart::k, T).size();
		mink = 0;
		maxk = (int64_t)d - 1;
		dict_k = true;
	}
	const uint64_t RX = (uint64_t)(maxx - minx) + 1, RK = (uint64_t)(maxk - mink) + 1;
	if (RX > (1ull << 31) - 1 || RK > (1ull << 31) - 1) {
		throw std::runtime_error("tessera: more than 2^31 - 1 distinct join keys or group keys");
	}
	check();
	double tb1e = Now();
	vector<int32_t> rank_of(RX, -1);
	{
		vector<uint32_t> cnt(RX, 0);
		vector<vector<uint32_t>> h(P);
		OomFlag oom;
#pragma omp parallel for num_threads(T) schedule(dynamic, 1)
		for (int p = 0; p < P; p++) {
			if (!oom.run([&] { h[p].assign(RX, 0); })) {
				continue;
			}
			const auto &px = parts[p]->x;
			for (size_t e = 0; e < px.size() && !poll((int64_t)e); e++) {
				h[p][px[e] - minx]++;
			}
		}
		oom.check("item counts");
		check();
#pragma omp parallel for num_threads(T) schedule(static)
		for (int64_t i = 0; i < (int64_t)RX; i++) {
			uint32_t s = 0;
			for (int p = 0; p < P; p++) {
				s += h[p][i];
			}
			cnt[i] = s;
		}
		vector<std::pair<int64_t, int64_t>> order;
		for (uint64_t i = 0; i < RX; i++) {
			if (cnt[i]) {
				order.push_back({-(int64_t)cnt[i], (int64_t)i + minx});
			}
		}
		ParSort(order, T);
		qr.orig.resize(order.size());
		for (size_t r = 0; r < order.size(); r++) {
			qr.orig[r] = dictx.empty() ? order[r].second : dictx[order[r].second];
			rank_of[order[r].second - minx] = (int32_t)r;
		}
		X.n_cols = (int32_t)order.size();
	}
	double tb2 = Now();
	check();
	X.n_rows = (int32_t)RK;
	double tb3 = 0;
	{
		vector<std::atomic<uint32_t>> rc(RK);
#pragma omp parallel for num_threads(T) schedule(static)
		for (int64_t i = 0; i < (int64_t)RK; i++) {
			rc[i].store(0, std::memory_order_relaxed);
		}
#pragma omp parallel for num_threads(T) schedule(dynamic, 1)
		for (int p = 0; p < P; p++) {
			const auto &pk = parts[p]->k;
			for (size_t e = 0; e < pk.size() && !poll((int64_t)e); e++) {
				rc[pk[e] - mink].fetch_add(1, std::memory_order_relaxed);
			}
		}
		check();
		vector<int64_t> ptr(RK + 1, 0);
		for (uint64_t i = 0; i < RK; i++) {
			ptr[i + 1] = ptr[i] + rc[i].load(std::memory_order_relaxed);
		}
		if ((size_t)ptr[RK] != n) {
			throw std::runtime_error("tessera: a join key has 2^32 or more rows");
		}
		vector<std::atomic<int64_t>> pos(RK);
#pragma omp parallel for num_threads(T) schedule(static)
		for (int64_t i = 0; i < (int64_t)RK; i++) {
			pos[i].store(ptr[i], std::memory_order_relaxed);
		}
		vector<int32_t> idx(n);
		vector<double> val(has_v && !has_iv ? n : 0);
		vector<int64_t> ival(has_iv ? n : 0);
#pragma omp parallel for num_threads(T) schedule(dynamic, 1)
		for (int p = 0; p < P; p++) {
			auto &pk = parts[p]->k, &px = parts[p]->x;
			auto &pv = parts[p]->v;
			auto &piv = parts[p]->iv;
			for (size_t e = 0; e < pk.size() && !poll((int64_t)e); e++) {
				int64_t o = pos[pk[e] - mink].fetch_add(1, std::memory_order_relaxed);
				idx[o] = rank_of[px[e] - minx];
				if (has_iv) {
					ival[o] = piv[e];
				} else if (has_v) {
					val[o] = pv[e];
				}
			}
		}
		parts.clear();
		tb3 = Now();
		check();
		vector<int64_t> keep(RK + 1, 0);
		std::atomic<bool> dup {false};
		int64_t maxlen = 0;
		if (has_v) {
			for (uint64_t r = 0; r < RK; r++) {
				maxlen = std::max(maxlen, ptr[r + 1] - ptr[r]);
			}
		}
		vector<ProdRange> prs(T);
		vector<uint8_t> side_rank;
		if (has_iv && cross) {
			side_rank.resize(qr.orig.size());
			for (size_t r = 0; r < qr.orig.size(); r++) {
				side_rank[r] = qr.orig[r] >= cx.count;
			}
		}
		OomFlag oom;
#pragma omp parallel num_threads(T)
		{
		vector<std::pair<int32_t, double>> tmp;
		vector<std::pair<int32_t, int64_t>> tmpi;
		const bool ok = oom.run([&] {
			if (has_iv) {
				tmpi.reserve(maxlen);
			} else if (has_v) {
				tmp.reserve(maxlen);
			}
		});
		ProdRange &pr = prs[omp_get_thread_num()];
#pragma omp for schedule(dynamic, 1024)
		for (int64_t r = 0; r < (int64_t)RK; r++) {
			if (!ok || poll(r) || stopped.load(std::memory_order_relaxed)) {
				continue;
			}
			int64_t b = ptr[r], e = ptr[r + 1];
			if (has_iv) {
				tmpi.clear();
				for (int64_t q = b; q < e; q++) {
					tmpi.push_back({idx[q], ival[q]});
				}
				std::sort(tmpi.begin(), tmpi.end());
				for (int64_t q = b; q < e; q++) {
					idx[q] = tmpi[q - b].first;
					ival[q] = tmpi[q - b].second;
				}
				RowProducts(tmpi.data(), e - b, spec.mode, cross ? side_rank.data() : nullptr, pr);
			} else if (e - b > 1) {
				if (has_v) {
					tmp.clear();
					for (int64_t q = b; q < e; q++) {
						tmp.push_back({idx[q], val[q]});
					}
					std::sort(tmp.begin(), tmp.end());
					for (int64_t q = b; q < e; q++) {
						idx[q] = tmp[q - b].first;
						val[q] = tmp[q - b].second;
					}
				} else {
					std::sort(idx.begin() + b, idx.begin() + e);
				}
			}
			int64_t u = e > b ? 1 : 0;
			for (int64_t q = b + 1; q < e; q++) {
				u += idx[q] != idx[q - 1];
			}
			keep[r + 1] = u;
			if (u != e - b) {
				dup = true;
			}
		}
		}
		oom.check("row sort");
		check();
		if (has_iv) {
			ProdRange all;
			for (auto &p : prs) {
				all.add(p);
			}
			qr.prod_min = all.lo;
			qr.prod_max = all.hi;
			MergeRowsInt(X, T, RK, ptr, idx, ival, keep);
		} else {
			MergeRows(X, qr, T, RK, has_v, ptr, idx, val, keep, dup);
			if (int_in) {
				bool any = false;
				for (int64_t r = 0; r < X.n_rows && !any; r++) {
					const int64_t b = X.ptr[r], e = X.ptr[r + 1];
					if (!cross) {
						any = e - b >= (spec.mode == PairMode::LT ? 2 : 1);
						continue;
					}
					bool sides[2] = {false, false};
					for (int64_t q = b; q < e; q++) {
						sides[qr.orig[X.idx[q]] >= cx.count] = true;
					}
					any = sides[0] && sides[1];
				}
				if (any) {
					qr.prod_min = qr.prod_max = 1;
				}
			}
		}
	}
	double t1 = Now();
	check();
	const int64_t Hset = spec.frontier;
	const bool reorder = spec.reorder;
	std::string flog;
	double tf = 0;
	Options opt;
	Stats st;
	uint64_t spilled = 0, spill_failed = 0;
	const int simm = (int)spec.similarity;
	vector<double> sim_n;
	vector<int32_t> sim_lo;
	if (simm) {
		std::vector<std::vector<double>> sq(T, std::vector<double>(X.n_cols, 0.0));
#pragma omp parallel for num_threads(T) schedule(static)
		for (int64_t r = 0; r < X.n_rows; r++) {
			auto &q = sq[omp_get_thread_num()];
			for (int64_t e = X.ptr[r]; e < X.ptr[r + 1]; e++) {
				const double v = !X.wval.empty() ? X.wval[e] : !X.ival.empty() ? (double)X.ival[e]
				                 : !X.xval.empty() ? (double)X.xval[e] : !X.xval_wide.empty() ? (double)X.xval_wide[e]
				                 : X.val.empty() ? 1.0 : (double)X.val[e];
				q[X.idx[e]] += v * v;
			}
		}
		sim_n.assign(X.n_cols, 0.0);
		for (int t = 0; t < T; t++) {
			for (int32_t i = 0; i < X.n_cols; i++) {
				sim_n[i] += sq[t][i];
			}
		}
		const double t = spec.min_sim;
		if (X.val.empty() && X.ival.empty() && X.wval.empty() && X.xval.empty() && X.xval_wide.empty() && t > 0 &&
		    spec.similarity != Similarity::OVERLAP) {
			const double f = spec.similarity == Similarity::COSINE ? 1 / (t * t)
			                 : spec.similarity == Similarity::JACCARD ? 1 / t
			                                                          : (2 - t) / t;
			sim_lo.resize(X.n_cols);
			for (int32_t j = 0; j < X.n_cols; j++) {
				const double bound = sim_n[j] * f * (1 + 1e-12);
				sim_lo[j] = (int32_t)(std::lower_bound(sim_n.begin(), sim_n.end(), bound, std::greater<double>()) - sim_n.begin());
			}
		}
	}
	vector<uint8_t> side_of;
	vector<int32_t> lay_rank;
	if (cross) {
		side_of.resize(qr.orig.size());
		for (size_t r = 0; r < qr.orig.size(); r++) {
			side_of[r] = qr.orig[r] >= cx.count;
		}
	}
	const bool exact = !X.xval.empty() || !X.xval_wide.empty();
	vector<double> xcol;
	double xvmax = 0;
	bool exact_wide = false;
	if (exact) {
		std::vector<std::vector<double>> cs(T, std::vector<double>(X.n_cols, 0.0));
		std::vector<double> vm(T, 0.0);
#pragma omp parallel for num_threads(T) schedule(static)
		for (int64_t r = 0; r < X.n_rows; r++) {
			auto &c = cs[omp_get_thread_num()];
			double &m = vm[omp_get_thread_num()];
			for (int64_t e = X.ptr[r]; e < X.ptr[r + 1]; e++) {
				const double a = std::fabs(!X.xval.empty() ? (double)X.xval[e] : (double)X.xval_wide[e]);
				c[X.idx[e]] += a;
				m = std::max(m, a);
			}
		}
		const double eps = std::numeric_limits<double>::epsilon();
		const double up = 1 + ((double)X.idx.size() + T + 8) * eps;
		xcol.assign(X.n_cols, 0.0);
		double cmax = 0;
		for (int32_t i = 0; i < X.n_cols; i++) {
			for (int t = 0; t < T; t++) {
				xcol[i] += cs[t][i];
			}
			xcol[i] *= up;
			cmax = std::max(cmax, xcol[i]);
		}
		xvmax = *std::max_element(vm.begin(), vm.end()) * up;
		exact_wide = !X.xval_wide.empty() || xvmax * cmax * up >= 9223372036854775808.0;
	}
	qr.wide = exact && exact_wide && !simm;
	int32_t track_from = 0;
	bool cell_any = false;
	auto run = [&](const Csr &Y, uint64_t minv) {
		opt = Options();
		opt.threads = T;
		opt.checksum = false;
		opt.out = &qr.out;
		opt.reorder = reorder;
		opt.min_value = minv;
		opt.out_budget = spec.out_budget;
		opt.spill_dir = spec.spill_dir;
		opt.spill = spec.spill;
		opt.spill_mkdir = spec.spill_mkdir;
		opt.spill_limit = spec.spill_limit;
		opt.out_chunk = spec.out_chunk;
		opt.should_stop = spec.should_stop;
		opt.stop_ctx = spec.stop_ctx;
		opt.exact_wide = exact_wide;
		opt.exact_track_from = track_from;
		opt.exact_track_diag = spec.mode != PairMode::LT;
		if (simm) {
			opt.sim_measure = simm;
			opt.min_sim = spec.min_sim;
			opt.sim_diag = spec.mode != PairMode::LT;
			opt.sim_n = sim_n.data();
			opt.sim_lo = sim_lo.empty() ? nullptr : sim_lo.data();
			opt.min_value = 0;
		}
		int64_t H = Hset;
		if (qr.general || !X.ival.empty() || exact) {
			H = 0;
		} else if (H < 0) {
			double tf0 = Now();
			flog.clear();
			H = cross ? choose_frontier_cross(Y, side_of, T, env_cost_model(), verbose ? &flog : nullptr)
			          : choose_frontier(Y, T, env_cost_model(), verbose ? &flog : nullptr, Y.val.empty());
			tf = Now() - tf0;
		}
		opt.H = (int)std::min<int64_t>(H, Y.n_cols);
		if (opt.H > 0 && !Y.val.empty() &&
		    !dense_fits_int32(Y, (int)std::min<int64_t>((opt.H + 31) / 32 * 32, Y.n_cols))) {
			opt.H = 0;
		}
		qr.out.clear();
		if (!cross) {
			xtx(Y, opt, &st);
		} else {
			int32_t c0 = 0, c1 = 0;
			for (int32_t c = 0; c < opt.H; c++) {
				(side_of[c] ? c1 : c0)++;
			}
			int32_t hx = c0 / 32 * 32, hy = c1 / 32 * 32;
			if (hx == 0 || hy == 0) {
				hx = hy = 0;
			}
			int32_t l0 = 0;
			for (int32_t c = 0, s0 = 0; c < Y.n_cols; c++) {
				if (!side_of[c] && s0++ >= hx) {
					l0++;
				}
			}
			const int32_t hc = hx + hy;
			vector<int32_t> lay(Y.n_cols);
			lay_rank.assign(Y.n_cols, 0);
			for (int32_t c = 0, p0 = 0, p1 = hx, q0 = hc, q1 = hc + l0; c < Y.n_cols; c++) {
				int32_t &next = side_of[c] ? (p1 < hc ? p1 : q1) : (p0 < hx ? p0 : q0);
				lay[c] = next++;
				lay_rank[lay[c]] = c;
			}
			Csr Z = PermuteItems(Y, lay, T);
			opt.H = hc;
			opt.cross_hx = hx;
			opt.cross_lx_end = hc + l0;
			xtx(Z, opt, &st);
		}
		spilled += st.spilled_chunks;
		spill_failed += st.spill_failed;
		if (st.cancelled) {
			qr.out.clear();
			throw Cancelled();
		}
		if (st.out_of_memory) {
			qr.out.clear();
			throw OutOfMemory("tessera: out of memory (output chunks or per-thread buffers of the product)");
		}
		if (!st.spill_error.empty()) {
			qr.out.clear();
			throw SpillError(st.spill_error, st.spill_limit_hit);
		}
		if (st.cell_any) {
			if (!cell_any || st.cell_min < qr.cell_min) {
				qr.cell_min = st.cell_min;
			}
			if (!cell_any || st.cell_max > qr.cell_max) {
				qr.cell_max = st.cell_max;
			}
			cell_any = true;
		}
	};
	std::vector<double> smax;
	if ((spec.min_value > 0 || spec.topk > 0) && exact && !simm) {
		smax.assign((size_t)X.n_cols + 1, 0.0);
		for (int64_t i = (int64_t)X.n_cols - 1; i >= 0; i--) {
			smax[i] = std::max(smax[i + 1], xvmax * xcol[i] * (1 + std::numeric_limits<double>::epsilon()));
		}
	} else if ((spec.min_value > 0 || spec.topk > 0) && !qr.general && !simm) {
		std::vector<std::vector<double>> cs(T, std::vector<double>(X.n_cols, 0.0));
		double vmax = 1;
		if (!X.val.empty()) {
			vmax = *std::max_element(X.val.begin(), X.val.end());
		}
		if (!X.ival.empty()) {
			vmax = *std::max_element(X.ival.begin(), X.ival.end());
		}
		auto value = [&](int64_t e) {
			return !X.ival.empty() ? (double)X.ival[e] : X.val.empty() ? 1.0 : (double)X.val[e];
		};
		double vmax_in[2] = {vmax, vmax};
		if (cross && (!X.val.empty() || !X.ival.empty())) {
			vmax_in[0] = vmax_in[1] = 1;
			for (size_t e = 0; e < X.idx.size(); e++) {
				double &m = vmax_in[side_of[X.idx[e]]];
				m = std::max(m, value((int64_t)e));
			}
		}
#pragma omp parallel for num_threads(T) schedule(static)
		for (int64_t r = 0; r < X.n_rows; r++) {
			auto &c = cs[omp_get_thread_num()];
			for (int64_t e = X.ptr[r]; e < X.ptr[r + 1]; e++) {
				c[X.idx[e]] += value(e);
			}
		}
		const double up = 1 + ((double)X.idx.size() + T + 8) * std::numeric_limits<double>::epsilon();
		smax.assign((size_t)X.n_cols + 1, 0.0);
		for (int64_t i = (int64_t)X.n_cols - 1; i >= 0; i--) {
			double b = 0;
			for (int t = 0; t < T; t++) {
				b += cs[t][i];
			}
			smax[i] = std::max(smax[i + 1], (cross ? vmax_in[1 - side_of[i]] : vmax) * b * up);
		}
	}
	auto run_prefix = [&](int32_t h, uint64_t minv) {
		if (h < X.n_cols) {
			Csr Y = RestrictItems(X, h, T);
			run(Y, minv);
		} else {
			run(X, minv);
		}
	};
	auto extend_prefix = [&](int32_t lo, int32_t h, uint64_t minv) {
		const Csr Y = RestrictItems(X, h, T, lo);
		vector<Cells> prev;
		prev.swap(qr.out);
		track_from = lo;
		run(Y, minv);
		track_from = 0;
		const size_t cw = qr.wide ? 2 : 1;
		for (auto &c : qr.out) {
			size_t w = 0;
			for (size_t q = 0; q < c.n; q += cw) {
				if (c.data[q].j >= lo) {
					for (size_t u = 0; u < cw && w != q; u++) {
						c.data[w + u] = c.data[q + u];
					}
					w += cw;
				}
			}
			c.n = w;
		}
		for (auto &c : prev) {
			qr.out.push_back(std::move(c));
		}
		return Y.n_rows;
	};
	auto prefix_for = [&](double tau) {
		return (int32_t)(std::find_if(smax.begin(), smax.end() - 1, [&](double b) { return b < tau; }) - smax.begin());
	};
	auto prune_floor = [&](uint64_t m) {
		return qr.int_values ? std::min((double)m, 9223372036854775808.0) : (double)m;
	};
	std::string plog;
	if (spec.min_value > 0 && spec.topk <= 0) {
		if (!smax.empty()) {
			const int32_t h = prefix_for(prune_floor(spec.min_value));
			plog += Fmt("having >= %llu: items %d of %d", (unsigned long long)spec.min_value, h, X.n_cols);
			run_prefix(h, spec.min_value);
		} else {
			run(X, spec.min_value);
		}
	} else if (spec.topk > 0) {
		const PairMode mode = spec.mode;
		const bool int_cells = !qr.general && !simm;
		__int128 tau_x = 0;
		bool full = false;
		auto kth_of = [&](auto zero) {
			using V = decltype(zero);
			std::vector<V> v;
			for (auto &c : qr.out) {
				for (size_t q = 0; q < cell_count(c, qr.wide); q++) {
					const auto &t = c.data[qr.wide ? 2 * q : q];
					const V x = (V)cell_value128(c, q, qr.wide);
					if (cross) {
						v.push_back(x);
					} else if (t.i == t.j) {
						if (mode != PairMode::LT) {
							v.push_back(x);
						}
					} else {
						v.push_back(x);
						if (mode == PairMode::FULL) {
							v.push_back(x);
						}
					}
				}
			}
			full = (int64_t)v.size() >= spec.topk;
			if (!full) {
				return (__int128)0;
			}
			std::nth_element(v.begin(), v.begin() + (spec.topk - 1), v.end(), std::greater<V>());
			return (__int128)v[spec.topk - 1];
		};
		auto kth_exact = [&]() {
			tau_x = qr.wide ? kth_of((__int128)0) : kth_of((int64_t)0);
			if (!full) {
				return 0.0;
			}
			return tau_x > 0 ? (double)tau_x * (1 - std::numeric_limits<double>::epsilon()) : (double)tau_x;
		};
		auto key = [](double x) { return std::isnan(x) ? HUGE_VAL : x; };
		auto kth = [&]() {
			if (int_cells) {
				return kth_exact();
			}
			std::vector<double> v;
			for (auto &c : qr.out) {
				for (size_t q = 0; q < c.n; q++) {
					const auto &t = c.data[q];
					const double x = key(cell_value(t.s, true));
					if (cross) {
						v.push_back(x);
					} else if (t.i == t.j) {
						if (mode != PairMode::LT) {
							v.push_back(x);
						}
					} else {
						v.push_back(x);
						if (mode == PairMode::FULL) {
							v.push_back(x);
						}
					}
				}
			}
			full = (int64_t)v.size() >= spec.topk;
			if (!full) {
				return 0.0;
			}
			std::nth_element(v.begin(), v.begin() + (spec.topk - 1), v.end(), std::greater<double>());
			return v[spec.topk - 1];
		};
		double tau = 0;
		const uint64_t floor_min = spec.min_value;
		if (!smax.empty()) {
			const int32_t hmax = floor_min > 0 ? prefix_for(prune_floor(floor_min)) : X.n_cols;
			auto cells = [&](double h) {
				return cross ? h * h / 4 : mode == PairMode::LT ? h * (h - 1) / 2 : mode == PairMode::LE ? h * (h + 1) / 2 : h * h;
			};
			int32_t h0 = 1;
			while (h0 < hmax && cells(h0) < (double)spec.topk) {
				h0++;
			}
			if (spec.topk_first > 0) {
				h0 = spec.topk_first;
			} else if (spec.topk_first == 0) {
				h0 = std::max(h0, TOPK_FIRST);
			}
			h0 = std::min(h0, hmax);
			run_prefix(h0, floor_min);
			tau = kth();
			while (spec.topk_first <= 0 && !full && h0 < hmax) {
				h0 = (int32_t)std::min<int64_t>(hmax, 2 * (int64_t)h0);
				run_prefix(h0, floor_min);
				tau = kth();
			}
			const double bound = std::max(full ? tau : 0.0, prune_floor(floor_min));
			int32_t h1 = bound > 0 ? prefix_for(bound) : X.n_cols;
			plog += Fmt("top-%lld: pass 1 items %d tau1 %.0f", (long long)spec.topk, h0, tau);
			if (h1 > h0) {
				const uint64_t tv = !full || tau_x <= 0 ? 0 : tau_x > (__int128)UINT64_MAX ? UINT64_MAX : (uint64_t)tau_x;
				const uint64_t minv = std::max<uint64_t>(floor_min, tv);
				if (cross) {
					run_prefix(h1, minv);
					plog += Fmt(", pass 2 items %d", h1);
				} else {
					const int32_t rows = extend_prefix(h0, h1, minv);
					plog += Fmt(", pass 2 items %d to %d rows %d of %d", h0, h1, rows, X.n_rows);
				}
				tau = kth();
				plog += Fmt(" tau %.0f", tau);
			}
		} else {
			run(X, floor_min);
			tau = kth();
		}
		if (full && int_cells) {
			const size_t cw = qr.wide ? 2 : 1;
			for (auto &c : qr.out) {
				size_t w = 0;
				for (size_t q = 0; q < cell_count(c, qr.wide); q++) {
					if (cell_value128(c, q, qr.wide) >= tau_x) {
						for (size_t u = 0; u < cw && w != cw * q; u++) {
							c.data[w + u] = c.data[cw * q + u];
						}
						w += cw;
					}
				}
				c.n = w;
			}
		} else if (full) {
			for (auto &c : qr.out) {
				size_t w = 0;
				for (size_t q = 0; q < c.n; q++) {
					if (key(cell_value(c.data[q].s, true)) >= tau) {
						if (w != q) {
							c.data[w] = c.data[q];
						}
						w++;
					}
				}
				c.n = w;
			}
		}
	} else {
		run(X, 0);
	}
	if (simm) {
		qr.general = true;
	}

	if (cross && !lay_rank.empty()) {
		vector<int64_t> o(lay_rank.size());
		qr.side.resize(lay_rank.size());
		for (size_t q = 0; q < lay_rank.size(); q++) {
			const int32_t r = lay_rank[q];
			qr.side[q] = side_of[r];
			o[q] = side_of[r] ? cy.value(qr.orig[r] - cx.count) : cx.value(qr.orig[r]);
		}
		qr.orig.swap(o);
	}
	if (verbose) {
		size_t cells = 0;
		for (auto &o : qr.out) {
			cells += o.n;
		}
		if (!plog.empty()) {
			fprintf(stderr, "tessera: pushdown %s\n", plog.c_str());
		}
		if (!flog.empty()) {
			fprintf(stderr, "tessera: frontier model (%.3f s)\n%s", tf, flog.c_str());
		}
		fprintf(stderr,
		        "tessera: build breakdown: minmax %.3f keys %.3f (%s x, %s k) ranks %.3f csr-scatter %.3f sort-merge %.3f s\n",
		        tb1 - t0, tb1e - tb1, dictx.empty() ? "direct" : "dictionary", dict_k ? "dictionary" : "direct",
		        tb2 - tb1e, tb3 - tb2, t1 - tb3);
		if (spilled || spill_failed) {
			fprintf(stderr, "tessera: output chunks spilled %llu (spill files failed, kept in memory: %llu)\n",
			        (unsigned long long)spilled, (unsigned long long)spill_failed);
		}
		fprintf(stderr,
		        "tessera: n %zu rows %d items %d H %d | build %.3f s, order %.3f, panels %.3f, dense %.3f, sparse %.3f, "
		        "total %.3f | tdpbuud %llu panels %llu block_pairs %llu sparse_products %llu cells(i<=j) %zu | colidx %.3f "
		        "hl %.3f hl_tdpbuud %llu grid_rows %llu hl_rows %llu hl_build %.3f\n",
		        n, X.n_rows, X.n_cols, opt.H, t1 - t0, st.t_order, st.t_panels, st.t_dense, st.t_sparse,
		        Now() - t0, (unsigned long long)st.tdpbuud, (unsigned long long)st.panels,
		        (unsigned long long)st.block_pairs, (unsigned long long)st.sparse_products, cells, st.t_colidx, st.t_hl,
		        (unsigned long long)st.hl_tdpbuud, (unsigned long long)st.grid_rows, (unsigned long long)st.hl_rows,
		        st.t_hl_build);
	}
	return;
}

void run_query(std::vector<std::unique_ptr<QueryPart>> &parts, const QuerySpec &spec, QueryResult &qr) {
	RunQuery(parts, nullptr, spec, qr);
}

void run_query_cross(std::vector<std::unique_ptr<QueryPart>> &parts_x, std::vector<std::unique_ptr<QueryPart>> &parts_y,
                     const QuerySpec &spec, QueryResult &qr) {
	RunQuery(parts_x, &parts_y, spec, qr);
}

}
