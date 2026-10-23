#include "query.hpp"

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
#include <parallel/algorithm>
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
		return (double)s;
	}
	double d;
	memcpy(&d, &s, sizeof d);
	return d;
}

static const int64_t POLL_EVERY = 1 << 16;

static const double RANGE_PER_DISTINCT = 4;

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
#pragma omp parallel num_threads(T)
	{
		I64Table set(1 << 16);
		for (auto &p : parts) {
			const auto &c = (*p).*col;
#pragma omp for schedule(static) nowait
			for (int64_t e = 0; e < (int64_t)c.size(); e++) {
				set.Insert(c[e]);
			}
		}
		auto &f = found[omp_get_thread_num()];
		f.reserve(set.n);
		for (size_t s = 0; s < set.key.size(); s++) {
			if (set.val[s] >= 0) {
				f.push_back(set.key[s]);
			}
		}
	}
	vector<int64_t> d;
	for (auto &f : found) {
		d.insert(d.end(), f.begin(), f.end());
		vector<int64_t>().swap(f);
	}
	__gnu_parallel::sort(d.begin(), d.end());
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
#pragma omp parallel num_threads(T)
	{
		vector<std::pair<int32_t, int64_t>> tmp;
#pragma omp for schedule(dynamic, 1024)
		for (int64_t r = 0; r < X.n_rows; r++) {
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
			}
		}
	}
	return Z;
}

struct Team {
	int saved;
	bool release;
	bool pinned = false;
	cpu_set_t caller_mask;
	Team(int T, bool rel) : saved(omp_get_max_threads()), release(rel) {
		vector<int> cpus;
		const bool env_bind = omp_get_proc_bind() != omp_proc_bind_false || getenv("GOMP_CPU_AFFINITY") != nullptr;
		if (!env_bind && sched_getaffinity(0, sizeof caller_mask, &caller_mask) == 0) {
			const int me = sched_getcpu();
			if (me >= 0 && CPU_ISSET(me, &caller_mask)) {
				cpus.push_back(me);
			}
			for (int c = 0; c < CPU_SETSIZE; c++) {
				if (CPU_ISSET(c, &caller_mask) && c != me) {
					cpus.push_back(c);
				}
			}
			pinned = cpus.size() > 1;
		}
		sigset_t all, old;
		sigfillset(&all);
		pthread_sigmask(SIG_BLOCK, &all, &old);
#pragma omp parallel num_threads(T)
		{
			if (pinned) {
				cpu_set_t one;
				CPU_ZERO(&one);
				CPU_SET(cpus[omp_get_thread_num() % cpus.size()], &one);
				pthread_setaffinity_np(pthread_self(), sizeof one, &one);
			}
		}
		pthread_sigmask(SIG_SETMASK, &old, nullptr);
		omp_set_num_threads(T);
	}
	~Team() {
		omp_set_num_threads(saved);
		if (pinned) {
			pthread_setaffinity_np(pthread_self(), sizeof caller_mask, &caller_mask);
		}
		if (release) {
			omp_pause_resource_all(omp_pause_soft);
		}
	}
};

static Csr RestrictItems(const Csr &X, int32_t h, int T) {
	Csr Y;
	Y.n_rows = X.n_rows;
	Y.n_cols = std::min(X.n_cols, h);
	Y.ptr.assign((size_t)X.n_rows + 1, 0);
#pragma omp parallel for num_threads(T) schedule(static)
	for (int64_t r = 0; r < X.n_rows; r++) {
		Y.ptr[r + 1] = std::lower_bound(X.idx.begin() + X.ptr[r], X.idx.begin() + X.ptr[r + 1], h) - (X.idx.begin() + X.ptr[r]);
	}
	for (int64_t r = 0; r < X.n_rows; r++) {
		Y.ptr[r + 1] += Y.ptr[r];
	}
	Y.idx.resize(Y.ptr[X.n_rows]);
	if (!X.val.empty()) {
		Y.val.resize(Y.idx.size());
	}
	if (!X.ival.empty()) {
		Y.ival.resize(Y.idx.size());
	}
	if (!X.wval.empty()) {
		Y.wval.resize(Y.idx.size());
	}
#pragma omp parallel for num_threads(T) schedule(static)
	for (int64_t r = 0; r < X.n_rows; r++) {
		const int64_t n = Y.ptr[r + 1] - Y.ptr[r];
		std::copy(X.idx.begin() + X.ptr[r], X.idx.begin() + X.ptr[r] + n, Y.idx.begin() + Y.ptr[r]);
		if (!X.val.empty()) {
			std::copy(X.val.begin() + X.ptr[r], X.val.begin() + X.ptr[r] + n, Y.val.begin() + Y.ptr[r]);
		}
		if (!X.ival.empty()) {
			std::copy(X.ival.begin() + X.ptr[r], X.ival.begin() + X.ptr[r] + n, Y.ival.begin() + Y.ptr[r]);
		}
		if (!X.wval.empty()) {
			std::copy(X.wval.begin() + X.ptr[r], X.wval.begin() + X.ptr[r] + n, Y.wval.begin() + Y.ptr[r]);
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

static void RunQuery(std::vector<std::unique_ptr<QueryPart>> &parts, std::vector<std::unique_ptr<QueryPart>> *parts_y,
                     const QuerySpec &spec, QueryResult &qr) {
	const int T = spec.threads > 0 ? spec.threads : omp_get_max_threads();
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
		n += p->k.size();
	}
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
		if (has_value) {
			for (auto v : parts[p]->v) {
				ones = ones && v == 1;
			}
		}
	}
	const bool has_v = has_value && !ones;
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
#pragma omp parallel for num_threads(T) schedule(dynamic, 1)
		for (int p = 0; p < P; p++) {
			h[p].assign(RX, 0);
			const auto &px = parts[p]->x;
			for (size_t e = 0; e < px.size() && !poll((int64_t)e); e++) {
				h[p][px[e] - minx]++;
			}
		}
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
		__gnu_parallel::sort(order.begin(), order.end());
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
		vector<std::atomic<int64_t>> pos(RK);
#pragma omp parallel for num_threads(T) schedule(static)
		for (int64_t i = 0; i < (int64_t)RK; i++) {
			pos[i].store(ptr[i], std::memory_order_relaxed);
		}
		vector<int32_t> idx(n);
		vector<double> val(has_v ? n : 0);
#pragma omp parallel for num_threads(T) schedule(dynamic, 1)
		for (int p = 0; p < P; p++) {
			auto &pk = parts[p]->k, &px = parts[p]->x;
			auto &pv = parts[p]->v;
			for (size_t e = 0; e < pk.size() && !poll((int64_t)e); e++) {
				int64_t o = pos[pk[e] - mink].fetch_add(1, std::memory_order_relaxed);
				idx[o] = rank_of[px[e] - minx];
				if (has_v) {
					val[o] = pv[e];
				}
			}
		}
		parts.clear();
		tb3 = Now();
		check();
		vector<int64_t> keep(RK + 1, 0);
		std::atomic<bool> dup {false};
#pragma omp parallel num_threads(T)
		{
		vector<std::pair<int32_t, double>> tmp;
#pragma omp for schedule(dynamic, 1024)
		for (int64_t r = 0; r < (int64_t)RK; r++) {
			if (poll(r) || stopped.load(std::memory_order_relaxed)) {
				continue;
			}
			int64_t b = ptr[r], e = ptr[r + 1];
			if (e - b > 1) {
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
		check();
		MergeRows(X, qr, T, RK, has_v, ptr, idx, val, keep, dup);
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
		if (X.val.empty() && X.ival.empty() && X.wval.empty() && t > 0 && spec.similarity != Similarity::OVERLAP) {
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
	auto run = [&](const Csr &Y, uint64_t minv) {
		opt = Options();
		opt.threads = T;
		opt.checksum = false;
		opt.out = &qr.out;
		opt.reorder = reorder;
		opt.min_value = minv;
		opt.out_budget = spec.out_budget;
		opt.spill_dir = spec.spill_dir;
		opt.should_stop = spec.should_stop;
		opt.stop_ctx = spec.stop_ctx;
		if (simm) {
			opt.sim_measure = simm;
			opt.min_sim = spec.min_sim;
			opt.sim_diag = spec.mode != PairMode::LT;
			opt.sim_n = sim_n.data();
			opt.sim_lo = sim_lo.empty() ? nullptr : sim_lo.data();
			opt.min_value = 0;
		}
		int64_t H = Hset;
		if (qr.general || !X.ival.empty()) {
			H = 0;
		} else if (H < 0) {
			double tf0 = Now();
			flog.clear();
			H = cross ? choose_frontier_cross(Y, side_of, T, CostModel(), verbose ? &flog : nullptr)
			          : choose_frontier(Y, T, CostModel(), verbose ? &flog : nullptr, Y.val.empty());
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
	};
	std::vector<double> smax;
	if ((spec.min_value > 0 || spec.topk > 0) && !qr.general && !simm) {
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
		smax.assign((size_t)X.n_cols + 1, 0.0);
		for (int64_t i = (int64_t)X.n_cols - 1; i >= 0; i--) {
			double b = 0;
			for (int t = 0; t < T; t++) {
				b += cs[t][i];
			}
			smax[i] = std::max(smax[i + 1], (cross ? vmax_in[1 - side_of[i]] : vmax) * b);
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
	auto prefix_for = [&](double tau) {
		return (int32_t)(std::find_if(smax.begin(), smax.end() - 1, [&](double b) { return b < tau; }) - smax.begin());
	};
	std::string plog;
	if (spec.min_value > 0 && spec.topk <= 0) {
		if (!smax.empty()) {
			const int32_t h = prefix_for((double)spec.min_value);
			plog += Fmt("having >= %llu: items %d of %d", (unsigned long long)spec.min_value, h, X.n_cols);
			run_prefix(h, spec.min_value);
		} else {
			run(X, spec.min_value);
		}
	} else if (spec.topk > 0) {
		const PairMode mode = spec.mode;
		auto kth = [&]() {
			std::vector<double> v;
			for (auto &c : qr.out) {
				for (size_t q = 0; q < c.n; q++) {
					const auto &t = c.data[q];
					const double x = cell_value(t.s, qr.general || simm);
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
			if ((int64_t)v.size() < spec.topk) {
				return 0.0;
			}
			std::nth_element(v.begin(), v.begin() + (spec.topk - 1), v.end(), std::greater<double>());
			return v[spec.topk - 1];
		};
		double tau = 0;
		const uint64_t floor_min = spec.min_value;
		if (!smax.empty()) {
			int32_t h0 = std::min<int32_t>(X.n_cols, 1024);
			run_prefix(h0, floor_min);
			tau = kth();
			int32_t h1 = tau > 0 ? prefix_for(tau) : X.n_cols;
			plog += Fmt("top-%lld: pass 1 items %d tau1 %.0f", (long long)spec.topk, h0, tau);
			if (h1 > h0) {
				run_prefix(h1, std::max<uint64_t>(floor_min, (uint64_t)tau));
				tau = kth();
				plog += Fmt(", pass 2 items %d tau %.0f", h1, tau);
			}
		} else {
			run(X, floor_min);
			tau = kth();
		}
		if (tau > 0) {
			for (auto &c : qr.out) {
				size_t w = 0;
				for (size_t q = 0; q < c.n; q++) {
					if (cell_value(c.data[q].s, qr.general || simm) >= tau) {
						c.data[w++] = c.data[q];
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
		        "total %.3f | tdpbuud %lu panels %lu block_pairs %lu sparse_products %lu cells(i<=j) %zu | colidx %.3f hl %.3f "
		        "hl_tdpbuud %lu\n",
		        n, X.n_rows, X.n_cols, opt.H, t1 - t0, st.t_order, st.t_panels, st.t_dense, st.t_sparse,
		        Now() - t0, st.tdpbuud, st.panels, st.block_pairs, st.sparse_products, cells, st.t_colidx, st.t_hl,
		        st.hl_tdpbuud);
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
