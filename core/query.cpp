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
#include <cstdlib>
#include <cstring>
#include <functional>
#include <set>
#include <limits>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
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

static const int kCodeBits = 40;
static const uint64_t kCodeMask = (1ull << kCodeBits) - 1;

static uint64_t BytesHash(const char *s, size_t len) {
	uint64_t h = (uint64_t)len * 0x9E3779B97F4A7C15ull;
	for (size_t i = 0; i < len; i += 8) {
		uint64_t w = 0;
		memcpy(&w, s + i, std::min<size_t>(8, len - i));
		h = (h ^ w) * 0x9E3779B97F4A7C15ull;
		h ^= h >> 32;
	}
	return h;
}

void StrDict::grow() {
	vector<uint64_t> t(std::max<size_t>(2, slot_.size() * 2), 0);
	const uint64_t mask = t.size() - 1;
	for (size_t c = 0; c < size(); c++) {
		const uint64_t h = BytesHash(bytes_.data() + off_[c], (size_t)(off_[c + 1] - off_[c]));
		uint64_t i = h & mask;
		while (t[i]) {
			i = (i + 1) & mask;
		}
		t[i] = (h >> kCodeBits << kCodeBits) | (uint64_t)(c + 1);
	}
	slot_.swap(t);
}

int64_t StrDict::code(const char *s, size_t len) {
	const uint64_t h = BytesHash(s, len), tag = h >> kCodeBits << kCodeBits;
	uint64_t mask = slot_.size() - 1, i = h & mask;
	for (; !slot_.empty() && slot_[i]; i = (i + 1) & mask) {
		if ((slot_[i] & ~kCodeMask) == tag) {
			const size_t c = (size_t)(slot_[i] & kCodeMask) - 1;
			if (off_[c + 1] - off_[c] == len && (len == 0 || memcmp(bytes_.data() + off_[c], s, len) == 0)) {
				return (int64_t)c;
			}
		}
	}
	const size_t c = size();
	if ((uint64_t)c >= kCodeMask) {
		throw std::length_error("tessera: a dictionary of more than 2^40 strings");
	}
	if ((c + 1) * 2 > slot_.size()) {
		grow();
		mask = slot_.size() - 1;
		for (i = h & mask; slot_[i]; i = (i + 1) & mask) {
		}
	}
	off_.push_back(off_.back() + len);
	try {
		bytes_.insert(bytes_.end(), s, s + len);
	} catch (...) {
		off_.pop_back();
		throw;
	}
	slot_[i] = tag | (uint64_t)(c + 1);
	return (int64_t)c;
}

bool StrDict::set_order(const std::vector<int64_t> &rank) {
	if (rank.size() != size()) {
		return false;
	}
	vector<char> seen(rank.size(), 0);
	for (int64_t r : rank) {
		if (r < 0 || (size_t)r >= rank.size() || seen[(size_t)r]) {
			return false;
		}
		seen[(size_t)r] = 1;
	}
	rank_ = rank;
	return true;
}

std::vector<int64_t> StrDict::finish() {
	const size_t n = size();
	vector<int64_t> order(n), old2new(n);
	for (size_t c = 0; c < n; c++) {
		order[c] = (int64_t)c;
	}
	if (!rank_.empty() && rank_.size() != n) {
		throw std::runtime_error("tessera: a dictionary's order covers fewer strings than it holds");
	}
	if (rank_.size() == n && n > 0) {
		for (size_t c = 0; c < n; c++) {
			order[(size_t)rank_[c]] = (int64_t)c;
		}
	} else {
		const char *b = bytes_.data();
		const uint64_t *o = off_.data();
		std::sort(order.begin(), order.end(), [b, o](int64_t x, int64_t y) {
			const size_t lx = (size_t)(o[x + 1] - o[x]), ly = (size_t)(o[y + 1] - o[y]), l = std::min(lx, ly);
			const int r = l ? memcmp(b + o[x], b + o[y], l) : 0;
			return r < 0 || (r == 0 && lx < ly);
		});
	}
	rank_.clear();
	vector<char> bytes(bytes_.size());
	vector<uint64_t> off(n + 1, 0);
	for (size_t c = 0; c < n; c++) {
		const size_t x = (size_t)order[c], l = (size_t)(off_[x + 1] - off_[x]);
		old2new[x] = (int64_t)c;
		if (l) {
			memcpy(bytes.data() + off[c], bytes_.data() + off_[x], l);
		}
		off[c + 1] = off[c] + l;
	}
	for (uint64_t &e : slot_) {
		if (e) {
			e = (e & ~kCodeMask) | (uint64_t)(old2new[(size_t)(e & kCodeMask) - 1] + 1);
		}
	}
	bytes_.swap(bytes);
	off_.swap(off);
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

static int OverflowFlags(bool int_values, __int128 prod_min, __int128 prod_max, __int128 cell_min, __int128 cell_max,
                         int product_bits, int sum_bits) {
	if (!int_values) {
		return 0;
	}
	auto outside = [](__int128 lo, __int128 hi, int bits) {
		if (bits <= 0 || bits >= 128) {
			return false;
		}
		const __int128 top = ((__int128)1 << (bits - 1)) - 1;
		return lo < -top - 1 || hi > top;
	};
	return (outside(prod_min, prod_max, product_bits) ? 1 : 0) | (outside(cell_min, cell_max, sum_bits) ? 2 : 0);
}

int overflow_flags(const QueryResult &r, int product_bits, int sum_bits) {
	return OverflowFlags(r.int_values, r.prod_min, r.prod_max, r.cell_min, r.cell_max, product_bits, sum_bits);
}

int overflow_flags(const QueryResult::SumResult &r, int product_bits, int sum_bits) {
	return OverflowFlags(r.int_values, r.prod_min, r.prod_max, r.cell_min, r.cell_max, product_bits, sum_bits);
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
static const size_t PIECE_ROWS = 1 << 20;
static size_t piece_rows() {
	const char *e = getenv("TESSERA_PIECE_ROWS");
	const long long v = e ? atoll(e) : 0;
	return v > 0 ? (size_t)v : PIECE_ROWS;
}

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

#ifdef __linux__
__attribute__((constructor)) static void unbind_loading_thread() {
	if (omp_get_proc_bind() == omp_proc_bind_false) {
		return;
	}
	const int places = omp_get_num_places();
	cpu_set_t cur, first, all;
	if (places < 2 || sched_getaffinity(0, sizeof cur, &cur) != 0) {
		return;
	}
	CPU_ZERO(&first);
	CPU_ZERO(&all);
	vector<int> ids;
	for (int p = 0; p < places; p++) {
		ids.assign((size_t)std::max(omp_get_place_num_procs(p), 0), -1);
		if (!ids.empty()) {
			omp_get_place_proc_ids(p, ids.data());
		}
		for (const int c : ids) {
			if (c >= 0 && c < CPU_SETSIZE) {
				CPU_SET(c, &all);
				if (p == 0) {
					CPU_SET(c, &first);
				}
			}
		}
	}
	if (CPU_EQUAL(&cur, &first)) {
		sched_setaffinity(0, sizeof all, &all);
	}
}
#endif
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
	bool restore = false;
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
		} else if (env_bind) {
			restore = sched_getaffinity(0, sizeof caller_mask, &caller_mask) == 0;
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
		if (pinned || restore) {
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
                      const vector<int32_t> &idx, const vector<double> &val, const vector<int64_t> &keep, bool dup,
                      bool exact_counts) {
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
		if (!ints && !has_v && exact_counts) {
			X.xval.resize(mv.size());
#pragma omp parallel for num_threads(T) schedule(static)
			for (int64_t q = 0; q < (int64_t)mv.size(); q++) {
				X.xval[q] = (int64_t)mv[q];
			}
		} else if (!ints) {
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
		bool fits = true;
#pragma omp parallel for num_threads(T) reduction(&& : fits)
		for (int64_t q = 0; q < (int64_t)X.xval_wide.size(); q++) {
			fits = fits && X.xval_wide[q] >= INT64_MIN && X.xval_wide[q] <= INT64_MAX;
		}
		if (!fits) {
			return;
		}
		mv.resize(X.xval_wide.size());
#pragma omp parallel for num_threads(T) schedule(static)
		for (int64_t q = 0; q < (int64_t)mv.size(); q++) {
			mv[q] = (int64_t)X.xval_wide[q];
		}
		vector<__int128>().swap(X.xval_wide);
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
	if (has_diag(mode)) {
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

static void ExactRowProducts(const std::pair<int32_t, int64_t> *t, int64_t m, const uint8_t *side, __int128 lo,
                             __int128 hi, const std::function<int64_t(int32_t)> &val,
                             bool (*pass)(void *, int64_t, int64_t), void *ctx, ProdRange &pr) {
	struct It {
		int32_t g;
		int64_t mx, mn;
	};
	vector<It> its;
	for (int64_t q = 0; q < m; q++) {
		if (its.empty() || its.back().g != t[q].first) {
			its.push_back({t[q].first, t[q].second, t[q].second});
		} else {
			its.back().mx = std::max(its.back().mx, t[q].second);
			its.back().mn = std::min(its.back().mn, t[q].second);
		}
	}
	std::set<std::pair<int32_t, int32_t>> tried;
	auto offer = [&](const It &a, const It &b, __int128 p) {
		const bool sa = side ? side[a.g] != 0 : false;
		const It &x = side && sa ? b : a, &y = side && sa ? a : b;
		auto key = std::make_pair(std::min(x.g, y.g), std::max(x.g, y.g));
		if (!tried.insert(key).second) {
			return false;
		}
		if (pass(ctx, val(x.g), val(y.g))) {
			pr.add(p);
			return true;
		}
		return false;
	};
	vector<int32_t> A, B;
	for (int32_t i = 0; i < (int32_t)its.size(); i++) {
		if (!side || side[its[i].g] == 0) {
			A.push_back(i);
		}
		if (!side || side[its[i].g] != 0) {
			B.push_back(i);
		}
	}
	auto scan = [&](bool amax, bool bmax, bool high) {
		auto f = [&](int32_t i, bool mx) { return mx ? its[i].mx : its[i].mn; };
		auto beyond = [&](__int128 p) { return high ? p > hi : p < lo; };
		vector<int32_t> a = A, b = B;
		std::sort(a.begin(), a.end(), [&](int32_t u, int32_t v) { return amax ? f(u, true) > f(v, true) : f(u, false) < f(v, false); });
		std::sort(b.begin(), b.end(), [&](int32_t u, int32_t v) { return bmax ? f(u, true) > f(v, true) : f(u, false) < f(v, false); });
		for (int32_t i : a) {
			bool any = false;
			for (int32_t j : b) {
				const __int128 p = (__int128)f(i, amax) * f(j, bmax);
				if (!beyond(p)) {
					break;
				}
				any = true;
				if (offer(its[i], its[j], p)) {
					return true;
				}
			}
			if (!any) {
				break;
			}
		}
		return false;
	};
	if (!scan(true, true, true) && !scan(false, false, true) && !scan(true, false, false)) {
		scan(false, true, false);
	}
}


template <class V, class Value>
static void KeepGroupTop(QueryResult &qr, const QuerySpec &spec, int T, bool cross, Value value) {
	const size_t items = qr.orig.size(), k = (size_t)spec.group_topk;
	size_t cells = 0;
	for (auto &c : qr.out) {
		cells += cell_count(c, qr.wide);
	}
	if (items == 0 || cells <= k || k > (size_t(1) << 40) / items) {
		return;
	}
	std::unique_ptr<V[]> heap;
	std::unique_ptr<std::atomic<V>[]> low;
	std::unique_ptr<std::atomic<uint32_t>[]> count;
	std::unique_ptr<std::atomic_flag[]> locks;
	const size_t nlocks = size_t(1) << 12;
	try {
		heap.reset(new V[items * k]);
		low.reset(new std::atomic<V>[items]);
		count.reset(new std::atomic<uint32_t>[items]);
		locks.reset(new std::atomic_flag[nlocks]);
	} catch (std::bad_alloc &) {
		return;
	}
	for (size_t g = 0; g < items; g++) {
		count[g].store(0, std::memory_order_relaxed);
	}
	for (size_t l = 0; l < nlocks; l++) {
		locks[l].clear();
	}
	const PairMode mode = spec.mode;
	const GroupOf of = spec.group_of;
	const int64_t *orig = qr.orig.data();
	const uint8_t *side = cross ? qr.side.data() : nullptr;
	auto groups = [&](const Triple &t, int32_t *g) -> int {
		if (cross) {
			const bool sw = side[t.i] != 0;
			g[0] = (of == GroupOf::FIRST) != sw ? t.i : t.j;
			return 1;
		}
		if (t.i == t.j) {
			g[0] = t.i;
			return has_diag(mode) ? 1 : 0;
		}
		if (of == GroupOf::EACH) {
			g[0] = t.i;
			g[1] = t.j;
			return 2;
		}
		g[0] = (orig[t.i] < orig[t.j]) == (of == GroupOf::LOWER) ? t.i : t.j;
		return 1;
	};
	auto offer = [&](size_t g, V x) {
		if (count[g].load(std::memory_order_acquire) == k && x < low[g].load(std::memory_order_relaxed)) {
			return;
		}
		std::atomic_flag &lock = locks[g & (nlocks - 1)];
		while (lock.test_and_set(std::memory_order_acquire)) {
		}
		V *h = heap.get() + g * k;
		const uint32_t n = count[g].load(std::memory_order_relaxed);
		if (n < k) {
			h[n] = x;
			if (n + 1 == k) {
				std::make_heap(h, h + k, std::greater<V>());
				low[g].store(h[0], std::memory_order_relaxed);
			}
			count[g].store(n + 1, std::memory_order_release);
		} else if (x > h[0]) {
			std::pop_heap(h, h + k, std::greater<V>());
			h[k - 1] = x;
			std::push_heap(h, h + k, std::greater<V>());
			low[g].store(h[0], std::memory_order_relaxed);
		}
		lock.clear(std::memory_order_release);
	};
	const int64_t nc = (int64_t)qr.out.size();
	const size_t cw = qr.wide ? 2 : 1;
#pragma omp parallel for schedule(dynamic, 1) num_threads(T)
	for (int64_t c = 0; c < nc; c++) {
		const Cells &o = qr.out[c];
		const size_t n = cell_count(o, qr.wide);
		for (size_t q = 0; q < n; q++) {
			int32_t g[2];
			const int m = groups(o.data[cw * q], g);
			if (m > 0) {
				const V x = value(o, q);
				for (int e = 0; e < m; e++) {
					offer(g[e], x);
				}
			}
		}
	}
#pragma omp parallel for schedule(dynamic, 1) num_threads(T)
	for (int64_t c = 0; c < nc; c++) {
		Cells &o = qr.out[c];
		const size_t n = cell_count(o, qr.wide);
		size_t w = 0;
		for (size_t q = 0; q < n; q++) {
			int32_t g[2];
			const int m = groups(o.data[cw * q], g);
			const V x = value(o, q);
			bool keep = false;
			for (int e = 0; e < m && !keep; e++) {
				keep = count[g[e]].load(std::memory_order_relaxed) < k || x >= heap[(size_t)g[e] * k];
			}
			if (keep) {
				for (size_t u = 0; u < cw && w != cw * q; u++) {
					o.data[w + u] = o.data[cw * q + u];
				}
				w += cw;
			}
		}
		o.n = w;
	}
}

static void KeepGroupTopWide(QueryResult &qr, const QuerySpec &spec, int T, bool cross) {
	const size_t items = qr.orig.size(), k = (size_t)spec.group_topk;
	size_t cells = 0;
	for (auto &c : qr.out) {
		cells += cell_count(c, true);
	}
	if (items == 0 || cells <= k || k > (size_t(1) << 40) / items) {
		return;
	}
	(void)T;
	std::vector<std::vector<__int128>> heap;
	try {
		heap.resize(items);
		for (auto &o : qr.out) {
			const size_t n = cell_count(o, true);
			for (size_t q = 0; q < n; q++) {
				const Triple &t = o.data[2 * q];
				int32_t g[2];
				int m = 1;
				if (cross) {
					const bool sw = qr.side[t.i] != 0;
					g[0] = (spec.group_of == GroupOf::FIRST) != sw ? t.i : t.j;
				} else if (t.i == t.j) {
					g[0] = t.i;
					m = has_diag(spec.mode) ? 1 : 0;
				} else if (spec.group_of == GroupOf::EACH) {
					g[0] = t.i;
					g[1] = t.j;
					m = 2;
				} else {
					g[0] = (qr.orig[t.i] < qr.orig[t.j]) == (spec.group_of == GroupOf::LOWER) ? t.i : t.j;
				}
				const __int128 x = cell_value128(o, q, true);
				for (int e = 0; e < m; e++) {
					auto &h = heap[g[e]];
					if (h.size() < k) {
						h.push_back(x);
						if (h.size() == k) {
							std::make_heap(h.begin(), h.end(), std::greater<__int128>());
						}
					} else if (x > h[0]) {
						std::pop_heap(h.begin(), h.end(), std::greater<__int128>());
						h[k - 1] = x;
						std::push_heap(h.begin(), h.end(), std::greater<__int128>());
					}
				}
			}
		}
	} catch (std::bad_alloc &) {
		return;
	}
	for (auto &o : qr.out) {
		const size_t n = cell_count(o, true);
		size_t w = 0;
		for (size_t q = 0; q < n; q++) {
			const Triple &t = o.data[2 * q];
			const __int128 x = cell_value128(o, q, true);
			int32_t g[2];
			int m = 1;
			if (cross) {
				const bool sw = qr.side[t.i] != 0;
				g[0] = (spec.group_of == GroupOf::FIRST) != sw ? t.i : t.j;
			} else if (t.i == t.j) {
				g[0] = t.i;
				m = has_diag(spec.mode) ? 1 : 0;
			} else if (spec.group_of == GroupOf::EACH) {
				g[0] = t.i;
				g[1] = t.j;
				m = 2;
			} else {
				g[0] = (qr.orig[t.i] < qr.orig[t.j]) == (spec.group_of == GroupOf::LOWER) ? t.i : t.j;
			}
			bool keep = false;
			for (int e = 0; e < m && !keep; e++) {
				keep = heap[g[e]].size() < k || x >= heap[g[e]][0];
			}
			if (keep) {
				for (size_t u = 0; u < 2 && w != 2 * q; u++) {
					o.data[w + u] = o.data[2 * q + u];
				}
				w += 2;
			}
		}
		o.n = w;
	}
}

struct ValueRange {
	double max_abs = 0, min_abs = HUGE_VAL;
	bool nonfinite = false;
	uint64_t rows = 0;
	double bound() const {
		return max_abs * max_abs * (double)rows * (double)rows;
	}
	bool may_overflow() const {
		return !(bound() < std::numeric_limits<double>::max());
	}
	bool may_underflow() const {
		return min_abs * min_abs == 0;
	}
	bool accum_may_overflow() const {
		const double tmp = 2 * bound();
		return !(tmp * tmp * (double)rows * (double)rows < std::numeric_limits<double>::max());
	}
};

static void AddRange(const std::vector<std::unique_ptr<QueryPart>> &parts, int T, ValueRange &r) {
	double mx = r.max_abs, mn = r.min_abs;
	bool nf = r.nonfinite;
	for (auto &p : parts) {
		r.rows += p->k.size();
		const double *v = p->v.data();
		const int64_t *iv = p->iv.data();
		const int64_t nv = (int64_t)p->v.size(), ni = (int64_t)p->iv.size();
#pragma omp parallel for num_threads(T) schedule(static) reduction(max : mx) reduction(min : mn) reduction(|| : nf)
		for (int64_t e = 0; e < nv; e++) {
			if (!std::isfinite(v[e])) {
				nf = true;
				continue;
			}
			const double a = std::fabs(v[e]);
			mx = std::max(mx, a);
			if (a > 0) {
				mn = std::min(mn, a);
			}
		}
#pragma omp parallel for num_threads(T) schedule(static) reduction(max : mx)
		for (int64_t e = 0; e < ni; e++) {
			mx = std::max(mx, std::fabs((double)iv[e]));
		}
	}
	r.max_abs = mx;
	r.min_abs = mn;
	r.nonfinite = nf;
}

static bool PruningUnsafe(const QuerySpec &spec, const ValueRange &r) {
	return (spec.float_errors && (r.may_overflow() || r.may_underflow() || (spec.float_accum && r.accum_may_overflow()))) ||
	       (spec.sum_limit > 0 && !(r.bound() < spec.sum_limit));
}

static int RowFloatErrors(double a, double b) {
	uint64_t ba, bb;
	memcpy(&ba, &a, sizeof a);
	memcpy(&bb, &b, sizeof b);
	if (ba == ROW_VALUE_OVERFLOW || bb == ROW_VALUE_OVERFLOW) {
		return 1;
	}
	if (ba == ROW_VALUE_UNDERFLOW || bb == ROW_VALUE_UNDERFLOW) {
		return 2;
	}
	return 0;
}

static int PairFloatErrors(double a, double b) {
	if (const int f = RowFloatErrors(a, b)) {
		return f;
	}
	const double p = a * b;
	return (std::isinf(p) && !std::isinf(a) && !std::isinf(b) ? 1 : 0) | (p == 0 && a != 0 && b != 0 ? 2 : 0);
}

static int SumFloatError(double before, double term) {
	return std::isinf(before + term) && !std::isinf(before) && !std::isinf(term) ? 1 : 0;
}

struct AccumCell {
	double sum = 0, n = 0, sxx = 0;
	int add(double v) {
		const double before = sum, m = n;
		n += 1;
		sum += v;
		if (m > 0) {
			const double tmp = v * n - sum;
			sxx += tmp * tmp / (n * m);
			if (std::isinf(sum) || std::isinf(sxx)) {
				const bool raise = !std::isinf(before) && !std::isinf(v);
				sxx = std::numeric_limits<double>::quiet_NaN();
				return raise ? 1 : 0;
			}
		} else if (!std::isfinite(v)) {
			sxx = std::numeric_limits<double>::quiet_NaN();
		}
		return 0;
	}
};

static void SumPairByPair(const vector<int64_t> &ptr, const vector<int32_t> &idx, const vector<double> &val, uint64_t RK,
                          PairMode mode, const uint8_t *side, const std::function<int64_t(int32_t)> &value,
                          const QuerySpec &spec, const std::function<void()> &check, QueryResult &qr) {
	try {
		std::unordered_map<uint64_t, AccumCell> cells;
		for (uint64_t r = 0; r < RK; r++) {
			if ((r & (POLL_EVERY - 1)) == 0) {
				check();
			}
			for (int64_t a = ptr[r]; a < ptr[r + 1]; a++) {
				for (int64_t b = ptr[r]; b < ptr[r + 1]; b++) {
					const int32_t i = idx[a], j = idx[b];
					if (side ? side[i] != 0 || side[j] == 0 : i > j || (i == j && !has_diag(mode))) {
						continue;
					}
					AccumCell &cell = cells[((uint64_t)(uint32_t)std::min(i, j) << 32) | (uint32_t)std::max(i, j)];
					const double term = val[a] * val[b];
					int f = 0;
					if (spec.float_accum) {
						f = cell.add(term);
					} else {
						f = SumFloatError(cell.sum, term);
						cell.sum += term;
					}
					if (spec.float_errors) {
						f |= PairFloatErrors(val[a], val[b]);
						if ((f & ~qr.float_errors) && (!spec.pair_pass || spec.pair_pass(spec.pair_ctx, value(i), value(j)))) {
							qr.float_errors |= f;
						}
					}
				}
			}
			if (qr.float_errors) {
				return;
			}
		}
		auto key = [](double x) { return std::isnan(x) ? HUGE_VAL : x; };
		double tau = spec.min_value > 0 ? (double)spec.min_value : -HUGE_VAL;
		if (spec.topk > 0) {
			vector<double> kept;
			for (auto &c : cells) {
				if (key(c.second.sum) >= tau) {
					kept.push_back(key(c.second.sum));
				}
			}
			if ((int64_t)kept.size() >= spec.topk) {
				std::nth_element(kept.begin(), kept.begin() + (spec.topk - 1), kept.end(), std::greater<double>());
				tau = kept[spec.topk - 1];
			}
		}
		if (tau > -HUGE_VAL) {
			for (auto it = cells.begin(); it != cells.end();) {
				it = key(it->second.sum) >= tau ? std::next(it) : cells.erase(it);
			}
		}
		const size_t chunk = size_t(1) << 20;
		qr.out.clear();
		size_t room = 0;
		Triple *t = nullptr;
		for (auto &c : cells) {
			if (room == 0) {
				room = std::min(chunk, cells.size() - qr.out.size() * chunk);
				qr.out.emplace_back();
				qr.out.back().data = ChunkPtr(new Triple[room], ChunkFree(room * sizeof(Triple)));
				qr.out.back().n = room;
				t = qr.out.back().data.get();
			}
			t->i = (int32_t)(c.first >> 32);
			t->j = (int32_t)(uint32_t)c.first;
			memcpy(&t->s, &c.second.sum, sizeof c.second.sum);
			t++;
			room--;
		}
	} catch (const std::bad_alloc &) {
		qr.out.clear();
		throw OutOfMemory("tessera: out of memory (the cells of a sum computed pair by pair)");
	}
}

static void RunQuery(std::vector<std::unique_ptr<QueryPart>> &parts, std::vector<std::unique_ptr<QueryPart>> *parts_y,
                     QuerySpec spec, QueryResult &qr) {
	const int T = std::min(spec.threads > 0 ? spec.threads : omp_get_max_threads(), std::max(1, omp_get_num_procs()));
	const bool has_value = spec.has_value;
	const bool verbose = spec.verbose;
	Team team(T, spec.release_threads);
	const bool cross = parts_y != nullptr;
	if (cross && spec.similarity != Similarity::NONE) {
		throw std::runtime_error("tessera: similarity is defined for the self-join only");
	}
	if (spec.distinct_keys && (has_value || spec.similarity != Similarity::NONE)) {
		throw std::runtime_error("tessera: COUNT(DISTINCT k) counts keys, without values or similarity");
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
	ValueRange range;
	if (has_value) {
		AddRange(parts, T, range);
		if (PruningUnsafe(spec, range)) {
			spec.min_value = 0;
			spec.topk = -1;
			spec.group_topk = -1;
		}
	}
	qr.float_errors = 0;
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
	struct Piece {
		int p;
		size_t lo, hi;
	};
	vector<Piece> pieces;
	const size_t piece = piece_rows();
	for (int p = 0; p < P; p++) {
		const size_t m = parts[p]->k.size();
		for (size_t lo = 0; lo < m; lo += piece) {
			pieces.push_back({p, lo, std::min(m, lo + piece)});
		}
	}
	const int64_t NP = (int64_t)pieces.size();
	qr.wide = false;
	qr.general = false;
	bool ones = true;
#pragma omp parallel for num_threads(T) schedule(dynamic, 1) reduction(min : minx, mink) reduction(max : maxx, maxk) \
    reduction(&& : ones)
	for (int64_t q = 0; q < NP; q++) {
		const QueryPart &pt = *parts[pieces[q].p];
		for (size_t e = pieces[q].lo; e < pieces[q].hi; e++) {
			minx = std::min(minx, pt.x[e]);
			maxx = std::max(maxx, pt.x[e]);
			mink = std::min(mink, pt.k[e]);
			maxk = std::max(maxk, pt.k[e]);
		}
		if (has_value && int_in) {
			for (size_t e = pieces[q].lo; e < pieces[q].hi; e++) {
				ones = ones && pt.iv[e] == 1;
			}
		} else if (has_value) {
			for (size_t e = pieces[q].lo; e < pieces[q].hi; e++) {
				ones = ones && pt.v[e] == 1;
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
	if ((uint64_t)P * RX > (uint64_t)n) {
		vector<std::atomic<uint32_t>> cnt(RX);
#pragma omp parallel for num_threads(T) schedule(static)
		for (int64_t i = 0; i < (int64_t)RX; i++) {
			cnt[i].store(0, std::memory_order_relaxed);
		}
#pragma omp parallel for num_threads(T) schedule(dynamic, 1)
		for (int64_t q = 0; q < NP; q++) {
			const auto &px = parts[pieces[q].p]->x;
			for (size_t e = pieces[q].lo; e < pieces[q].hi && !poll((int64_t)e); e++) {
				cnt[px[e] - minx].fetch_add(1, std::memory_order_relaxed);
			}
		}
		check();
		uint32_t top = 0;
		for (uint64_t i = 0; i < RX; i++) {
			top = std::max(top, cnt[i].load(std::memory_order_relaxed));
		}
		vector<int64_t> first((size_t)top + 1, 0);
		for (uint64_t i = 0; i < RX; i++) {
			first[cnt[i].load(std::memory_order_relaxed)]++;
		}
		first[0] = 0;
		int64_t items = 0;
		for (int64_t c = top; c > 0; c--) {
			const int64_t m = first[c];
			first[c] = items;
			items += m;
		}
		qr.orig.resize(items);
		for (uint64_t i = 0; i < RX; i++) {
			const uint32_t c = cnt[i].load(std::memory_order_relaxed);
			if (c) {
				const int64_t r = first[c]++;
				const int64_t x = (int64_t)i + minx;
				qr.orig[r] = dictx.empty() ? x : dictx[x];
				rank_of[i] = (int32_t)r;
			}
		}
		X.n_cols = (int32_t)items;
	} else {
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
		for (int64_t q = 0; q < NP; q++) {
			const auto &pk = parts[pieces[q].p]->k;
			for (size_t e = pieces[q].lo; e < pieces[q].hi && !poll((int64_t)e); e++) {
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
		for (int64_t q = 0; q < NP; q++) {
			const int p = pieces[q].p;
			auto &pk = parts[p]->k, &px = parts[p]->x;
			auto &pv = parts[p]->v;
			auto &piv = parts[p]->iv;
			for (size_t e = pieces[q].lo; e < pieces[q].hi && !poll((int64_t)e); e++) {
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
		vector<vector<int64_t>> suspect(T);
		const bool exact_pairs = has_iv && spec.pair_pass && (cross || spec.mode == PairMode::FULL);
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
				if (exact_pairs) {
					ProdRange rp;
					RowProducts(tmpi.data(), e - b, spec.mode, cross ? side_rank.data() : nullptr, rp);
					if (rp.any && (rp.lo < spec.prod_lo || rp.hi > spec.prod_hi)) {
						suspect[omp_get_thread_num()].push_back(r);
					} else {
						pr.add(rp);
					}
				} else {
					RowProducts(tmpi.data(), e - b, spec.mode, cross ? side_rank.data() : nullptr, pr);
				}
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
			auto value = [&](int32_t r) {
				return !cross ? qr.orig[r] : side_rank[r] ? cy.value(qr.orig[r] - cx.count) : cx.value(qr.orig[r]);
			};
			for (auto &sl : suspect) {
				for (int64_t r : sl) {
					vector<std::pair<int32_t, int64_t>> row;
					for (int64_t q = ptr[r]; q < ptr[r + 1]; q++) {
						row.push_back({idx[q], ival[q]});
					}
					ExactRowProducts(row.data(), (int64_t)row.size(), cross ? side_rank.data() : nullptr, spec.prod_lo,
					                 spec.prod_hi, value, spec.pair_pass, spec.pair_ctx, all);
					check();
				}
			}
			qr.prod_min = all.lo;
			qr.prod_max = all.hi;
			MergeRowsInt(X, T, RK, ptr, idx, ival, keep);
		} else if (has_v && spec.similarity == Similarity::NONE &&
		           (((range.nonfinite || range.may_overflow()) && dup) ||
		            (spec.float_errors && (range.may_overflow() || range.may_underflow() ||
		                                   (spec.float_accum && range.accum_may_overflow()))))) {
			const size_t items = qr.orig.size();
			vector<uint8_t> sides(cross ? items : 0);
			vector<int64_t> values(items);
			for (size_t r = 0; r < items; r++) {
				if (cross) {
					sides[r] = qr.orig[r] >= cx.count;
				}
				values[r] = !cross ? qr.orig[r] : sides[r] ? cy.value(qr.orig[r] - cx.count) : cx.value(qr.orig[r]);
			}
			SumPairByPair(ptr, idx, val, RK, spec.mode, cross ? sides.data() : nullptr,
			              [&](int32_t r) { return values[r]; }, spec, check, qr);
			check();
			qr.general = true;
			if (cross) {
				qr.side = std::move(sides);
				qr.orig = std::move(values);
			}
			if (spec.group_topk > 0 && !qr.float_errors) {
				KeepGroupTop<double>(qr, spec, T, cross, [](const Cells &o, size_t q) {
					const double x = cell_value(o.data[q].s, true);
					return std::isnan(x) ? HUGE_VAL : x;
				});
			}
			return;
		} else {
			MergeRows(X, qr, T, RK, has_v, ptr, idx, val, keep, dup && !spec.distinct_keys,
			          spec.similarity == Similarity::NONE);
			if (int_in) {
				bool any = false;
				for (int64_t r = 0; r < X.n_rows && !any; r++) {
					const int64_t b = X.ptr[r], e = X.ptr[r + 1];
					if (!cross) {
						any = e - b >= (has_diag(spec.mode) ? 1 : 2);
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
		opt.exact_track_diag = has_diag(spec.mode);
		if (simm) {
			opt.sim_measure = simm;
			opt.min_sim = spec.min_sim;
			opt.sim_diag = has_diag(spec.mode);
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
						if (has_diag(mode)) {
							v.push_back(x);
						}
					} else {
						v.push_back(x);
						if (both_orders(mode)) {
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
						if (has_diag(mode)) {
							v.push_back(x);
						}
					} else {
						v.push_back(x);
						if (both_orders(mode)) {
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
				return cross ? h * h / 4 : mode == PairMode::LT ? h * (h - 1) / 2 : mode == PairMode::LE ? h * (h + 1) / 2 : mode == PairMode::NE ? h * (h - 1) : h * h;
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
	if (spec.group_topk > 0 && !simm) {
		const double t_group = Now();
		size_t before = 0, after = 0;
		for (auto &o : qr.out) {
			before += o.n;
		}
		if (qr.general) {
			KeepGroupTop<double>(qr, spec, T, cross, [](const Cells &o, size_t q) {
				const double x = cell_value(o.data[q].s, true);
				return std::isnan(x) ? HUGE_VAL : x;
			});
		} else if (qr.wide) {
			KeepGroupTopWide(qr, spec, T, cross);
		} else {
			KeepGroupTop<int64_t>(qr, spec, T, cross, [](const Cells &o, size_t q) { return (int64_t)o.data[q].s; });
		}
		for (auto &o : qr.out) {
			after += o.n;
		}
		plog += Fmt("%sgroup top-%lld: %zu of %zu cells (%.3f s)", plog.empty() ? "" : "; ", (long long)spec.group_topk,
		            after, before, Now() - t_group);
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

static void HeldBytes(const vector<Cells> &out, size_t &heap, uint64_t &spilled) {
	heap = 0;
	spilled = 0;
	for (auto &c : out) {
		const ChunkFree &f = c.data.get_deleter();
		if (f.map) {
			spilled += f.bytes;
		} else {
			heap += f.bytes;
		}
	}
}

void run_query_two(std::vector<std::unique_ptr<QueryPart>> &parts, std::vector<std::unique_ptr<QueryPart>> *parts_y,
                   const QuerySpec &spec, QueryResult &qr) {
	if (!spec.has_value || spec.similarity != Similarity::NONE) {
		throw std::runtime_error("tessera: two aggregates need values (COUNT and SUM), without similarity");
	}
	auto keys = [](const std::vector<std::unique_ptr<QueryPart>> &ps) {
		std::vector<std::unique_ptr<QueryPart>> c;
		for (auto &p : ps) {
			std::unique_ptr<QueryPart> q(new QueryPart());
			q->k = p->k;
			q->x = p->x;
			c.push_back(std::move(q));
		}
		return c;
	};
	std::vector<std::unique_ptr<QueryPart>> cx = keys(parts), cy;
	if (parts_y) {
		cy = keys(*parts_y);
	}
	QuerySpec cs = spec;
	cs.has_value = false;
	ValueRange range;
	const int TR = std::min(spec.threads > 0 ? spec.threads : omp_get_max_threads(), std::max(1, omp_get_num_procs()));
	AddRange(parts, TR, range);
	if (parts_y) {
		AddRange(*parts_y, TR, range);
	}
	if (PruningUnsafe(spec, range)) {
		cs.min_value = 0;
		cs.topk = -1;
		cs.group_topk = -1;
	}
	RunQuery(cx, parts_y ? &cy : nullptr, cs, qr);
	size_t heap1;
	uint64_t spill1;
	HeldBytes(qr.out, heap1, spill1);
	QuerySpec ss = spec;
	ss.min_value = 0;
	ss.topk = -1;
	ss.group_topk = -1;
	ss.distinct_keys = false;
	if (spec.out_budget) {
		ss.out_budget = spec.out_budget > heap1 ? spec.out_budget - heap1 : 1;
	}
	if (spec.spill_limit) {
		ss.spill_limit = spec.spill_limit > spill1 ? spec.spill_limit - spill1 : 1;
	}
	QueryResult r2;
	RunQuery(parts, parts_y, ss, r2);
	const int T = std::min(spec.threads > 0 ? spec.threads : omp_get_max_threads(), std::max(1, omp_get_num_procs()));
	Team team(T, spec.release_threads);
	const double tm = Now();
	qr.general2 = r2.general;
	qr.float_errors = r2.float_errors;
	qr.int_values = r2.int_values;
	qr.prod_min = r2.prod_min;
	qr.prod_max = r2.prod_max;
	qr.cell_min = r2.cell_min;
	qr.cell_max = r2.cell_max;
	std::atomic<bool> stopped {false};
	auto poll = [&]() {
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
	const size_t R = qr.orig.size();
	vector<int32_t> to;
	if (!(qr.orig == r2.orig && qr.side == r2.side)) {
		auto item = [](const QueryResult &r, size_t i) {
			return std::make_pair(r.side.empty() ? 0 : (int)r.side[i], r.orig[i]);
		};
		vector<std::pair<std::pair<int, int64_t>, int32_t>> by(R);
		for (size_t r = 0; r < R; r++) {
			by[r] = {item(qr, r), (int32_t)r};
		}
		std::sort(by.begin(), by.end());
		to.resize(r2.orig.size());
		for (size_t r = 0; r < r2.orig.size(); r++) {
			const auto key = item(r2, r);
			auto it = std::lower_bound(by.begin(), by.end(), std::make_pair(key, INT32_MIN));
			to[r] = it != by.end() && it->first == key ? it->second : -1;
		}
	}
	auto rank = [&](int32_t r) {
		return to.empty() ? r : to[r];
	};
	struct E {
		int32_t col;
		uint64_t lo;
		int64_t hi;
		bool operator<(const E &o) const {
			return col < o.col;
		}
	};
	const size_t NC = r2.out.size();
	size_t total = 0, qcells = 0;
	for (auto &c : r2.out) {
		total += cell_count(c, r2.wide);
	}
	for (auto &c : qr.out) {
		qcells += cell_count(c, qr.wide);
	}
	const int L = (int)std::max<size_t>(1, std::min<size_t>((size_t)T, total / std::max<size_t>(R, 1)));
	vector<size_t> cut(L + 1, NC);
	{
		size_t seen = 0, c = 0;
		for (int l = 0; l < L; l++) {
			cut[l] = c;
			while (c < NC && seen < total / L * (size_t)(l + 1)) {
				seen += cell_count(r2.out[c++], r2.wide);
			}
		}
	}
	vector<int64_t> pos((size_t)L * R, 0);
#pragma omp parallel for num_threads(T) schedule(static, 1)
	for (int l = 0; l < L; l++) {
		int64_t *h = pos.data() + (size_t)l * R;
		for (size_t c = cut[l]; c < cut[l + 1] && !poll(); c++) {
			for (size_t q = 0; q < cell_count(r2.out[c], r2.wide); q++) {
				const Triple &x = r2.out[c].data[r2.wide ? 2 * q : q];
				const int32_t ri = rank(x.i), rj = rank(x.j);
				if (ri >= 0 && rj >= 0) {
					h[std::min(ri, rj)]++;
				}
			}
		}
	}
	check();
	const double tm_count = Now();
	vector<int64_t> off(R + 1, 0);
#pragma omp parallel for num_threads(T) schedule(static)
	for (int64_t r = 0; r < (int64_t)R; r++) {
		int64_t s = 0;
		for (int l = 0; l < L; l++) {
			s += pos[(size_t)l * R + r];
		}
		off[r + 1] = s;
	}
	int64_t big = 0;
	for (size_t r = 0; r < R; r++) {
		big = std::max(big, off[r + 1]);
		off[r + 1] += off[r];
	}
#pragma omp parallel for num_threads(T) schedule(static)
	for (int64_t r = 0; r < (int64_t)R; r++) {
		int64_t p = off[r];
		for (int l = 0; l < L; l++) {
			const int64_t n = pos[(size_t)l * R + r];
			pos[(size_t)l * R + r] = p;
			p += n;
		}
	}
	size_t cap = (size_t)off[R];
	if (spec.out_budget) {
		size_t heap2;
		uint64_t spill2;
		HeldBytes(r2.out, heap2, spill2);
		const size_t used = heap1 + heap2 + qcells * sizeof(__int128);
		const size_t left = spec.out_budget > used ? spec.out_budget - used : 0;
		cap = std::min<size_t>((size_t)off[R], std::max<size_t>(left / sizeof(E), (size_t)big));
	}
	qr.out2.assign(qr.out.size(), std::vector<__int128>());
	OomFlag oom;
#pragma omp parallel for num_threads(T) schedule(dynamic, 1)
	for (int64_t c = 0; c < (int64_t)qr.out.size(); c++) {
		oom.run([&] { qr.out2[c].resize(cell_count(qr.out[c], qr.wide)); });
	}
	oom.check("second aggregate");
	std::unique_ptr<E[]> es(new E[std::max<size_t>(cap, 1)]);
	double t_scatter = 0, t_sort = 0, t_lookup = 0;
	int batches = 0;
	for (size_t r0 = 0; r0 < R;) {
		size_t r1 = r0 + 1;
		while (r1 < R && (size_t)(off[r1 + 1] - off[r0]) <= cap) {
			r1++;
		}
		const bool last = r1 == R;
		const int64_t b0 = off[r0];
		double t = Now();
#pragma omp parallel for num_threads(T) schedule(static, 1)
		for (int l = 0; l < L; l++) {
			int64_t *p = pos.data() + (size_t)l * R;
			for (size_t c = cut[l]; c < cut[l + 1] && !poll(); c++) {
				for (size_t q = 0; q < cell_count(r2.out[c], r2.wide); q++) {
					const Triple &x = r2.out[c].data[r2.wide ? 2 * q : q];
					const int32_t ri = rank(x.i), rj = rank(x.j), a = std::min(ri, rj);
					if (a < 0 || (size_t)a < r0 || (size_t)a >= r1) {
						continue;
					}
					const __int128 v = r2.general ? (__int128)r2.out[c].data[q].s : cell_value128(r2.out[c], q, r2.wide);
					E &e = es[p[a]++ - b0];
					e.col = std::max(ri, rj);
					e.lo = (uint64_t)v;
					e.hi = (int64_t)(v >> 64);
				}
				if (last) {
					r2.out[c] = Cells();
				}
			}
		}
		check();
		t_scatter += Now() - t;
		t = Now();
#pragma omp parallel for num_threads(T) schedule(dynamic, 1)
		for (int64_t r = (int64_t)r0; r < (int64_t)r1; r++) {
			std::sort(es.get() + (off[r] - b0), es.get() + (off[r + 1] - b0));
		}
		t_sort += Now() - t;
		t = Now();
#pragma omp parallel for num_threads(T) schedule(dynamic, 1)
		for (int64_t c = 0; c < (int64_t)qr.out.size(); c++) {
			if (poll()) {
				continue;
			}
			for (size_t q = 0; q < qr.out2[c].size(); q++) {
				const Triple &x = qr.out[c].data[qr.wide ? 2 * q : q];
				const int32_t a = std::min(x.i, x.j), bcol = std::max(x.i, x.j);
				if ((size_t)a < r0 || (size_t)a >= r1) {
					continue;
				}
				const E *b = es.get() + (off[a] - b0), *e = es.get() + (off[a + 1] - b0);
				const E *it = std::lower_bound(b, e, E {bcol, 0, 0});
				if (it != e && it->col == bcol) {
					qr.out2[c][q] = (__int128)(((unsigned __int128)(uint64_t)it->hi << 64) | it->lo);
				}
			}
		}
		check();
		t_lookup += Now() - t;
		batches++;
		r0 = r1;
	}
	r2.out.clear();
	if (spec.verbose) {
		fprintf(stderr,
		        "tessera: two aggregates: cells %zu, merged by rank in %.3f s (%d batches; count %.3f, scatter %.3f, sort "
		        "%.3f, lookup %.3f)%s\n",
		        (size_t)off[R], Now() - tm, batches, tm_count - tm, t_scatter, t_sort, t_lookup,
		        to.empty() ? "" : ", ranks translated");
	}
}

void run_query_many(std::vector<std::unique_ptr<QueryPart>> &parts, std::vector<std::unique_ptr<QueryPart>> &parts_y,
                    size_t n_sums, const QuerySpec &spec, QueryResult &qr) {
	if (n_sums == 0 || spec.similarity != Similarity::NONE) {
		throw std::runtime_error("tessera: many aggregates need sums, without similarity");
	}
	const int T = std::min(spec.threads > 0 ? spec.threads : omp_get_max_threads(), std::max(1, omp_get_num_procs()));
	Team team(T, spec.release_threads);
	const double t0 = Now();
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
	std::vector<uint8_t> is_int(n_sums, 0);
	size_t rows[2] = {0, 0};
	bool masked = false;
	for (int side = 0; side < 2; side++) {
		for (auto &p : side ? parts_y : parts) {
			if (p->x.size() != p->k.size()) {
				throw std::runtime_error("tessera: a part has group keys for some of its rows");
			}
			rows[side] += p->k.size();
			if (p->k.empty()) {
				continue;
			}
			if (p->sum_v.size() != n_sums || p->sum_iv.size() != n_sums) {
				throw std::runtime_error("tessera: a part has values for some of the sums");
			}
			for (size_t s = 0; s < n_sums; s++) {
				if (!p->sum_iv[s].empty()) {
					is_int[s] = 1;
				}
			}
			if (!p->sum_null.empty()) {
				if (p->sum_null.size() != p->k.size()) {
					throw std::runtime_error("tessera: a part has NULL marks for some of its rows");
				}
				masked = true;
			}
		}
	}
	for (int side = 0; side < 2; side++) {
		for (auto &p : side ? parts_y : parts) {
			for (size_t s = 0; s < n_sums && !p->k.empty(); s++) {
				if ((is_int[s] ? p->sum_iv[s].size() : p->sum_v[s].size()) != p->k.size()) {
					throw std::runtime_error("tessera: a part has values for some of its rows");
				}
			}
		}
	}
	if (masked && n_sums > 64) {
		throw std::runtime_error("tessera: NULL marks for more than 64 sums");
	}
	const uint64_t all_sums = n_sums >= 64 ? ~0ull : (1ull << n_sums) - 1;
	std::vector<size_t> slot(n_sums);
	size_t nd = 0, ni = 0, stride = 1;
	qr.sums.assign(n_sums, QueryResult::SumResult());
	for (size_t s = 0; s < n_sums; s++) {
		slot[s] = is_int[s] ? ni++ : nd++;
		qr.sums[s].at = stride;
		qr.sums[s].general = !is_int[s];
		qr.sums[s].int_values = is_int[s];
		stride += is_int[s] ? 2 : 1;
	}
	qr.null_at = 0;
	if (masked) {
		qr.null_at = stride++;
	}
	qr.stride = stride;
	qr.general = qr.wide = false;
	qr.float_errors = 0;
	qr.out.clear();
	std::vector<size_t> dsum(nd), isum(ni);
	std::vector<uint64_t> dbit(nd), ibit(ni);
	for (size_t s = 0; s < n_sums; s++) {
		(is_int[s] ? isum : dsum)[slot[s]] = s;
		(is_int[s] ? ibit : dbit)[slot[s]] = s < 64 ? 1ull << s : 0;
	}
	if (rows[0] == 0 || rows[1] == 0) {
		parts.clear();
		parts_y.clear();
		return;
	}
	SideCodes cx = EncodeSide(parts, T, poll);
	check();
	SideCodes cy = EncodeSide(parts_y, T, poll);
	check();
	int64_t mink = INT64_MAX, maxk = INT64_MIN;
	for (int side = 0; side < 2; side++) {
		for (auto &p : side ? parts_y : parts) {
			for (auto k : p->k) {
				mink = std::min(mink, k);
				maxk = std::max(maxk, k);
			}
		}
	}
	const uint64_t kspan = (uint64_t)maxk - (uint64_t)mink;
	size_t nkeys;
	const size_t nx_parts = parts.size();
	if (kspan < (1ull << 28) && (double)kspan + 1 <= RANGE_PER_DISTINCT * (double)(rows[0] + rows[1])) {
		nkeys = (size_t)kspan + 1;
		for (int side = 0; side < 2; side++) {
			for (auto &p : side ? parts_y : parts) {
#pragma omp parallel for num_threads(T) schedule(static)
				for (int64_t e = 0; e < (int64_t)p->k.size(); e++) {
					p->k[e] -= mink;
				}
			}
		}
	} else {
		std::vector<std::unique_ptr<QueryPart>> all;
		for (auto &p : parts) {
			all.push_back(std::move(p));
		}
		for (auto &p : parts_y) {
			all.push_back(std::move(p));
		}
		nkeys = Encode(all, &QueryPart::k, T).size();
		check();
		for (size_t p = 0; p < all.size(); p++) {
			(p < nx_parts ? parts[p] : parts_y[p - nx_parts]) = std::move(all[p]);
		}
	}
	const size_t nxi = (size_t)cx.count, nyi = (size_t)cy.count;
	if (nxi + nyi > (size_t)INT32_MAX) {
		throw std::runtime_error("tessera: more than 2^31 - 1 group keys");
	}
	std::unique_ptr<std::atomic<int64_t>[]> ox(new std::atomic<int64_t>[nxi + 1]), oy(new std::atomic<int64_t>[nkeys + 1]);
#pragma omp parallel for num_threads(T) schedule(static)
	for (int64_t i = 0; i <= (int64_t)nxi; i++) {
		ox[i].store(0, std::memory_order_relaxed);
	}
#pragma omp parallel for num_threads(T) schedule(static)
	for (int64_t i = 0; i <= (int64_t)nkeys; i++) {
		oy[i].store(0, std::memory_order_relaxed);
	}
	for (auto &p : parts) {
#pragma omp parallel for num_threads(T) schedule(static)
		for (int64_t e = 0; e < (int64_t)p->x.size(); e++) {
			ox[p->x[e]].fetch_add(1, std::memory_order_relaxed);
		}
	}
	for (auto &p : parts_y) {
#pragma omp parallel for num_threads(T) schedule(static)
		for (int64_t e = 0; e < (int64_t)p->k.size(); e++) {
			oy[p->k[e]].fetch_add(1, std::memory_order_relaxed);
		}
	}
	auto running = [](std::atomic<int64_t> *o, size_t n) {
		int64_t sum = 0;
		for (size_t i = 0; i <= n; i++) {
			const int64_t c = o[i].load(std::memory_order_relaxed);
			o[i].store(sum, std::memory_order_relaxed);
			sum += c;
		}
	};
	running(ox.get(), nxi);
	running(oy.get(), nkeys);
	std::vector<int32_t> xkey(rows[0]), yitem(rows[1]);
	std::vector<double> xd(rows[0] * nd), yd(rows[1] * nd);
	std::vector<int64_t> xi(rows[0] * ni), yi(rows[1] * ni);
	std::vector<uint64_t> xm(masked ? rows[0] : 0), ym(masked ? rows[1] : 0);
	for (int side = 0; side < 2; side++) {
		for (auto &p : side ? parts_y : parts) {
#pragma omp parallel for num_threads(T) schedule(static)
			for (int64_t e = 0; e < (int64_t)p->k.size(); e++) {
				const int64_t at = (side ? oy[p->k[e]] : ox[p->x[e]]).fetch_add(1, std::memory_order_relaxed);
				if (side) {
					yitem[at] = (int32_t)p->x[e];
				} else {
					xkey[at] = (int32_t)p->k[e];
				}
				for (size_t c = 0; c < nd; c++) {
					(side ? yd : xd)[(size_t)at * nd + c] = p->sum_v[dsum[c]][e];
				}
				for (size_t c = 0; c < ni; c++) {
					(side ? yi : xi)[(size_t)at * ni + c] = p->sum_iv[isum[c]][e];
				}
				if (masked) {
					(side ? ym : xm)[at] = p->sum_null.empty() ? 0 : p->sum_null[e] & all_sums;
				}
			}
		}
		(side ? parts_y : parts).clear();
	}
	check();
	const double t1 = Now();
	OutSpace space;
	space.budget = spec.out_budget;
	space.dir = spec.spill_dir;
	space.spill = spec.spill;
	space.mkdir_dir = spec.spill_mkdir;
	space.spill_limit = spec.spill_limit;
	space.chunk_cells = spec.out_chunk ? spec.out_chunk : std::max<size_t>(stride, (size_t(1) << 20));
	space.chunk_cells = std::max(stride, space.chunk_cells / stride * stride);
	space.lanes(T);
	std::vector<std::vector<Cells>> chunks(T);
	std::vector<uint64_t> products(T, 0);
	std::vector<std::vector<__int128>> ranges(T, std::vector<__int128>(ni * 4, 0));
	std::vector<std::vector<uint8_t>> ranged(T, std::vector<uint8_t>(ni * 2, 0));
	bool check_pairs = false;
	if (spec.float_errors && nd) {
		ValueRange vr;
		vr.rows = rows[0] + rows[1];
		double mx = 0, mn = HUGE_VAL;
		bool marked = false;
		const int64_t ntotal[2] = {(int64_t)xd.size(), (int64_t)yd.size()};
		const double *dv[2] = {xd.data(), yd.data()};
		for (int side = 0; side < 2; side++) {
			const double *d = dv[side];
#pragma omp parallel for num_threads(T) schedule(static) reduction(max : mx) reduction(min : mn) reduction(|| : marked)
			for (int64_t e = 0; e < ntotal[side]; e++) {
				uint64_t bits;
				memcpy(&bits, &d[e], sizeof bits);
				marked = marked || bits == ROW_VALUE_OVERFLOW || bits == ROW_VALUE_UNDERFLOW;
				if (std::isfinite(d[e])) {
					const double a = std::fabs(d[e]);
					mx = std::max(mx, a);
					if (a > 0) {
						mn = std::min(mn, a);
					}
				}
			}
		}
		vr.max_abs = mx;
		vr.min_abs = mn;
		check_pairs = marked || vr.may_overflow() || vr.may_underflow();
	}
	std::vector<int> pair_errors(T, 0);
	std::vector<std::set<std::tuple<int32_t, int32_t, int>>> error_pairs(T);
	std::vector<int64_t> yval;
	const int pair_op = spec.many_pair_op;
	if (pair_op) {
		yval.resize(nyi);
		for (size_t j = 0; j < nyi; j++) {
			yval[j] = cy.value((int64_t)j);
		}
	}
	OomFlag oom;
#pragma omp parallel num_threads(T)
	{
		const int tid = omp_get_thread_num();
		std::vector<double> ad;
		std::vector<__int128> ai;
		std::vector<uint64_t> cnt, had;
		std::vector<int32_t> touched;
		bool ok = oom.run([&] {
			ad.assign(nyi * nd, 0.0);
			ai.assign(nyi * ni, 0);
			cnt.assign(nyi, 0);
			had.assign(masked ? nyi : 0, 0);
			touched.reserve(1 << 16);
		});
		ChunkPtr spare;
		Triple *cur = nullptr, *end = nullptr;
		__int128 *rng = ranges[tid].data();
		uint8_t *seen = ranged[tid].data();
		auto close_chunk = [&]() {
			if (cur) {
				Cells &c = chunks[tid].back();
				c.n = (size_t)(cur - c.data.get());
				space.settle(c, spare, tid);
				cur = end = nullptr;
			}
		};
		auto open_chunk = [&]() {
			close_chunk();
			if (space.failed.load(std::memory_order_relaxed)) {
				return false;
			}
			try {
				chunks[tid].emplace_back();
				chunks[tid].back().data = spare ? std::move(spare) : space.alloc(space.chunk_cells);
			} catch (const std::bad_alloc &) {
				if (!chunks[tid].empty() && !chunks[tid].back().data) {
					chunks[tid].pop_back();
				}
				space.no_memory();
				return false;
			}
			cur = chunks[tid].back().data.get();
			end = cur + space.chunk_cells;
			return true;
		};
#pragma omp for schedule(dynamic, 16)
		for (int64_t i = 0; i < (int64_t)nxi; i++) {
			if (!ok || poll(i) || space.failed.load(std::memory_order_relaxed)) {
				continue;
			}
			const int64_t rb = i == 0 ? 0 : ox[i - 1].load(std::memory_order_relaxed), re = ox[i].load(std::memory_order_relaxed);
			uint64_t made = 0;
			const int64_t xv = pair_op ? cx.value(i) : 0;
			for (int64_t r = rb; r < re; r++) {
				const int32_t key = xkey[r];
				const int64_t b = key == 0 ? 0 : oy[key - 1].load(std::memory_order_relaxed), e = oy[key].load(std::memory_order_relaxed);
				const double *vd = nd ? xd.data() + (size_t)r * nd : nullptr;
				const int64_t *vi = ni ? xi.data() + (size_t)r * ni : nullptr;
				for (int64_t q = b; q < e; q++) {
					const int32_t j = yitem[q];
					if (pair_op) {
						const int64_t yv = yval[j];
						const bool pass = pair_op == 1   ? xv < yv
						                  : pair_op == 2 ? xv <= yv
						                  : pair_op == 3 ? xv > yv
						                  : pair_op == 4 ? xv >= yv
						                                 : xv != yv;
						if (!pass) {
							continue;
						}
					}
					if (cnt[j]++ == 0) {
						if (touched.size() == touched.capacity()) {
							ok = oom.run([&] { touched.reserve(touched.capacity() * 2); });
							if (!ok) {
								cnt[j] = 0;
								break;
							}
						}
						touched.push_back(j);
					}
					const uint64_t skip = masked ? xm[r] | ym[q] : 0;
					if (masked) {
						had[j] |= ~skip;
					}
					double *ac = nd ? ad.data() + (size_t)j * nd : nullptr;
					const double *wd = nd ? yd.data() + (size_t)q * nd : nullptr;
					int f = 0;
					if (check_pairs) {
						for (size_t c = 0; c < nd; c++) {
							f |= skip & dbit[c] ? RowFloatErrors(vd[c], wd[c])
							                    : PairFloatErrors(vd[c], wd[c]) | SumFloatError(ac[c], vd[c] * wd[c]);
						}
					}
					if (!skip) {
						for (size_t c = 0; c < nd; c++) {
							ac[c] += vd[c] * wd[c];
						}
					} else {
						for (size_t c = 0; c < nd; c++) {
							if (!(skip & dbit[c])) {
								ac[c] += vd[c] * wd[c];
							}
						}
					}
					if (check_pairs) {
						if (f && !spec.pair_pass) {
							pair_errors[tid] |= f;
						} else if (f) {
							ok = oom.run([&] { error_pairs[tid].insert(std::make_tuple((int32_t)i, j, f)); });
							if (!ok) {
								break;
							}
						}
					}
					if (ni) {
						__int128 *ax = ai.data() + (size_t)j * ni;
						const int64_t *wi = yi.data() + (size_t)q * ni;
						for (size_t c = 0; c < ni; c++) {
							if (skip & ibit[c]) {
								continue;
							}
							const __int128 pr = (__int128)vi[c] * wi[c];
							ax[c] += pr;
							if (!seen[2 * c] || pr < rng[4 * c]) {
								rng[4 * c] = pr;
							}
							if (!seen[2 * c] || pr > rng[4 * c + 1]) {
								rng[4 * c + 1] = pr;
							}
							seen[2 * c] = 1;
						}
					}
				}
				made += (uint64_t)(e - b);
			}
			products[tid] += made;
			for (int32_t j : touched) {
				const uint64_t nulls = masked ? all_sums & ~had[j] : 0;
				if (masked) {
					had[j] = 0;
				}
				if (cur == end && !open_chunk()) {
					cnt[j] = 0;
					continue;
				}
				cur[0].i = (int32_t)i;
				cur[0].j = (int32_t)nxi + j;
				cur[0].s = cnt[j];
				cnt[j] = 0;
				if (masked) {
					Triple *t = cur + qr.null_at;
					t[0].i = cur[0].i;
					t[0].j = cur[0].j;
					t[0].s = nulls;
				}
				for (size_t s = 0; s < n_sums; s++) {
					Triple *t = cur + qr.sums[s].at;
					t[0].i = cur[0].i;
					t[0].j = cur[0].j;
					if (!is_int[s]) {
						double &a = ad[(size_t)j * nd + slot[s]];
						memcpy(&t[0].s, &a, sizeof a);
						a = 0;
					} else {
						__int128 &a = ai[(size_t)j * ni + slot[s]];
						t[0].s = (uint64_t)a;
						t[1].i = t[0].i;
						t[1].j = t[0].j;
						t[1].s = (uint64_t)(a >> 64);
						const size_t c = slot[s];
						if (!(nulls >> s & 1)) {
							if (!seen[2 * c + 1] || a < rng[4 * c + 2]) {
								rng[4 * c + 2] = a;
							}
							if (!seen[2 * c + 1] || a > rng[4 * c + 3]) {
								rng[4 * c + 3] = a;
							}
							seen[2 * c + 1] = 1;
						}
						a = 0;
					}
				}
				cur += stride;
			}
			touched.clear();
		}
		close_chunk();
		space.release(spare);
	}
	oom.check("the sums' accumulators");
	check();
	if (space.oom.load()) {
		throw OutOfMemory("tessera: out of memory (output cells)");
	}
	if (space.failed.load()) {
		throw SpillError(space.error, space.limit_hit);
	}
	for (int t = 0; t < T; t++) {
		if (!space.map_spilled(chunks[t], t)) {
			throw SpillError(space.error, space.limit_hit);
		}
		for (auto &c : chunks[t]) {
			qr.out.push_back(std::move(c));
		}
	}
	for (int t = 0; t < T; t++) {
		qr.float_errors |= pair_errors[t];
		for (auto &e : error_pairs[t]) {
			if ((std::get<2>(e) & ~qr.float_errors) &&
			    spec.pair_pass(spec.pair_ctx, cx.value(std::get<0>(e)), cy.value(std::get<1>(e)))) {
				qr.float_errors |= std::get<2>(e);
			}
		}
	}
	for (size_t c = 0; c < ni; c++) {
		auto &r = qr.sums[isum[c]];
		bool have_p = false, have_c = false;
		for (int t = 0; t < T; t++) {
			if (ranged[t][2 * c]) {
				r.prod_min = have_p ? std::min(r.prod_min, ranges[t][4 * c]) : ranges[t][4 * c];
				r.prod_max = have_p ? std::max(r.prod_max, ranges[t][4 * c + 1]) : ranges[t][4 * c + 1];
				have_p = true;
			}
			if (ranged[t][2 * c + 1]) {
				r.cell_min = have_c ? std::min(r.cell_min, ranges[t][4 * c + 2]) : ranges[t][4 * c + 2];
				r.cell_max = have_c ? std::max(r.cell_max, ranges[t][4 * c + 3]) : ranges[t][4 * c + 3];
				have_c = true;
			}
		}
	}
	qr.orig.resize(nxi + nyi);
	qr.side.resize(nxi + nyi);
	for (size_t i = 0; i < nxi; i++) {
		qr.orig[i] = cx.value((int64_t)i);
		qr.side[i] = 0;
	}
	for (size_t j = 0; j < nyi; j++) {
		qr.orig[nxi + j] = cy.value((int64_t)j);
		qr.side[nxi + j] = 1;
	}
	if (spec.verbose) {
		uint64_t made = 0;
		size_t triples = 0;
		for (auto x : products) {
			made += x;
		}
		for (auto &c : qr.out) {
			triples += c.n;
		}
		fprintf(stderr, "tessera: %zu sums: %zu + %zu rows, build %.3f s, %llu products, %zu cells, total %.3f s\n", n_sums,
		        rows[0], rows[1], t1 - t0, (unsigned long long)made, triples / stride, Now() - t0);
	}
}

}
