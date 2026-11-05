#include "tessera_operator.hpp"

#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "tessera.hpp"
#include "query.hpp"

#include <omp.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace duckdb {

static_assert((int)TesseraPairMode::FULL == (int)tessera::PairMode::FULL && (int)TesseraPairMode::LT == (int)tessera::PairMode::LT &&
                  (int)TesseraPairMode::LE == (int)tessera::PairMode::LE && (int)TesseraPairMode::NE == (int)tessera::PairMode::NE,
              "TesseraPairMode and tessera::PairMode must agree");

static double TesseraNow() {
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

LogicalTessera::LogicalTessera(unique_ptr<LogicalAggregate> agg_op_p, unique_ptr<LogicalOperator> child,
                               TesseraPlanInfo info_p)
    : agg_op(std::move(agg_op_p)), info(std::move(info_p)) {
	children.push_back(std::move(child));
}

vector<ColumnBinding> LogicalTessera::GetColumnBindings() {
	vector<ColumnBinding> result;
	for (idx_t i = 0; i < agg_op->groups.size(); i++) {
		result.emplace_back(agg_op->group_index, ProjectionIndex(i));
	}
	for (idx_t i = 0; i < agg_op->expressions.size(); i++) {
		result.emplace_back(agg_op->aggregate_index, ProjectionIndex(i));
	}
	return result;
}

void LogicalTessera::ResolveTypes() {
	types.clear();
	for (auto &g : agg_op->groups) {
		types.push_back(g->GetReturnType());
	}
	for (auto &e : agg_op->expressions) {
		types.push_back(e->GetReturnType());
	}
}

PhysicalOperator &LogicalTessera::CreatePlan(ClientContext &context, PhysicalPlanGenerator &generator) {
	auto bindings = children[0]->GetColumnBindings();
	auto find = [&](const ColumnBinding &b) -> idx_t {
		for (idx_t i = 0; i < bindings.size(); i++) {
			if (bindings[i] == b) {
				return i;
			}
		}
		throw InternalException("tessera: column binding not found in the scan output");
	};
	TesseraPlanInfo resolved = info;
	if (info.cross) {
		resolved.k_pos = 1;
		resolved.x_pos = 2;
		resolved.v_pos = info.has_value ? 4 : DConstants::INVALID_INDEX;
	} else {
		auto pos = [&](idx_t e, const ColumnBinding &dflt) -> idx_t {
			if (expressions.empty()) {
				return find(dflt);
			}
			auto &ex = *expressions[e];
			return ex.GetExpressionClass() == ExpressionClass::BOUND_REF
			           ? ex.Cast<BoundReferenceExpression>().Index()
					   : find(ex.Cast<BoundColumnRefExpression>().Binding());
		};
		resolved.k_pos = pos(0, info.k_bind);
		resolved.x_pos = pos(1, info.x_bind);
		if (info.has_value) {
			resolved.v_pos = pos(2, info.v_bind);
		}
	}
	idx_t card = children[0]->EstimateCardinality(context);
	auto &child = generator.CreatePlan(*children[0]);
	return generator.Make<PhysicalTessera>(types, child, resolved, card);
}

PhysicalTessera::PhysicalTessera(PhysicalPlan &physical_plan, vector<LogicalType> types, PhysicalOperator &child,
                                 TesseraPlanInfo info_p, idx_t estimated_cardinality)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, std::move(types), estimated_cardinality),
      info(std::move(info_p)) {
	children.push_back(child);
}

class TesseraLocalSink : public LocalSinkState {
public:
	vector<int64_t> k, x;
	vector<double> v;
	vector<int64_t> k2, y;
	vector<double> w;
	vector<int64_t> iv, iw;
	bool null_x = false, bad_v = false;
	tessera::StrDict kd, xd, yd;
};

class TesseraGlobalSink : public GlobalSinkState {
public:
	mutex lock;
	std::vector<std::unique_ptr<tessera::QueryPart>> parts, parts_y;
	bool null_x = false, bad_v = false;
	tessera::StrDict kd, xd, yd;
	vector<uint8_t> side;
	vector<int64_t> orig;
	std::vector<tessera::Cells> out;
	std::vector<std::vector<__int128>> out2;
	bool general2 = false;
	bool general = false;
	bool wide = false;
	int threads = 1;
};

unique_ptr<GlobalSinkState> PhysicalTessera::GetGlobalSinkState(ClientContext &context) const {
	auto g = make_uniq<TesseraGlobalSink>();
	g->threads = (int)TaskScheduler::GetScheduler(context).NumberOfThreads();
	return std::move(g);
}

unique_ptr<LocalSinkState> PhysicalTessera::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq<TesseraLocalSink>();
}

static void AppendCodes(Vector &vec, idx_t n, const vector<bool> &keep, tessera::StrDict &d, vector<int64_t> &dst) {
	UnifiedVectorFormat f;
	vec.ToUnifiedFormat(f);
	auto s = UnifiedVectorFormat::GetData<string_t>(f);
	for (idx_t r = 0; r < n; r++) {
		if (keep[r]) {
			const string_t &v = s[f.sel->get_index(r)];
			dst.push_back(d.code(v.GetData(), v.GetSize()));
		}
	}
}

static Vector *AsBigint(ClientContext &ctx, Vector &vec, idx_t n, Vector &tmp) {
	switch (vec.GetType().id()) {
	case LogicalTypeId::BIGINT:
		return &vec;
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ: {
		UnifiedVectorFormat f;
		vec.ToUnifiedFormat(f);
		auto out = FlatVector::GetDataMutable<int64_t>(tmp);
		const bool i32 = vec.GetType().InternalType() == PhysicalType::INT32;
		for (idx_t r = 0; r < n; r++) {
			const idx_t i = f.sel->get_index(r);
			if (!f.validity.RowIsValid(i)) {
				FlatVector::SetNull(tmp, r, true);
				continue;
			}
			out[r] = i32 ? (int64_t)UnifiedVectorFormat::GetData<int32_t>(f)[i] : UnifiedVectorFormat::GetData<int64_t>(f)[i];
		}
		return &tmp;
	}
	default:
		VectorOperations::Cast(ctx, vec, tmp, n);
		return &tmp;
	}
}

static void AppendInt64(ClientContext &ctx, Vector &vec, idx_t n, const vector<bool> &keep, vector<int64_t> &dst) {
	Vector tmp(LogicalType::BIGINT, n);
	Vector *src = AsBigint(ctx, vec, n, tmp);
	UnifiedVectorFormat f;
	src->ToUnifiedFormat(f);
	auto d = UnifiedVectorFormat::GetData<int64_t>(f);
	for (idx_t r = 0; r < n; r++) {
		if (keep[r]) {
			dst.push_back(d[f.sel->get_index(r)]);
		}
	}
}

static void AppendInt64V(ClientContext &ctx, Vector &vec, idx_t n, const vector<bool> &keep, vector<int64_t> &dst,
                         bool &bad) {
	if (vec.GetType().id() == LogicalTypeId::DECIMAL) {
		UnifiedVectorFormat f;
		vec.ToUnifiedFormat(f);
		const PhysicalType pt = vec.GetType().InternalType();
		for (idx_t r = 0; r < n; r++) {
			if (!keep[r]) {
				continue;
			}
			const idx_t i = f.sel->get_index(r);
			if (!f.validity.RowIsValid(i)) {
				bad = true;
			}
			dst.push_back(pt == PhysicalType::INT16   ? (int64_t)UnifiedVectorFormat::GetData<int16_t>(f)[i]
			              : pt == PhysicalType::INT32 ? (int64_t)UnifiedVectorFormat::GetData<int32_t>(f)[i]
			                                          : UnifiedVectorFormat::GetData<int64_t>(f)[i]);
		}
		return;
	}
	Vector tmp(LogicalType::BIGINT, n);
	Vector *src = &vec;
	if (vec.GetType().id() != LogicalTypeId::BIGINT) {
		VectorOperations::Cast(ctx, vec, tmp, n);
		src = &tmp;
	}
	UnifiedVectorFormat f;
	src->ToUnifiedFormat(f);
	auto d = UnifiedVectorFormat::GetData<int64_t>(f);
	for (idx_t r = 0; r < n; r++) {
		if (keep[r]) {
			auto idx = f.sel->get_index(r);
			if (!f.validity.RowIsValid(idx)) {
				bad = true;
			}
			dst.push_back(d[idx]);
		}
	}
}

static void AppendDouble(ClientContext &ctx, Vector &vec, idx_t n, const vector<bool> &keep, vector<double> &dst, bool &bad) {
	Vector tmp(LogicalType::DOUBLE, n);
	Vector *src = &vec;
	if (vec.GetType().id() != LogicalTypeId::DOUBLE) {
		VectorOperations::Cast(ctx, vec, tmp, n);
		src = &tmp;
	}
	UnifiedVectorFormat f;
	src->ToUnifiedFormat(f);
	auto d = UnifiedVectorFormat::GetData<double>(f);
	for (idx_t r = 0; r < n; r++) {
		if (keep[r]) {
			auto idx = f.sel->get_index(r);
			if (!f.validity.RowIsValid(idx)) {
				bad = true;
			}
			dst.push_back(d[idx]);
		}
	}
}

static void AppendTupleCodes(ClientContext &ctx, DataChunk &chunk, idx_t first, idx_t ncol, const vector<bool> &strs,
                             idx_t n, const vector<bool> &keep, tessera::StrDict &d, vector<int64_t> &dst,
                             const vector<bool> *nulls = nullptr) {
	vector<Vector> casts;
	casts.reserve(ncol);
	vector<UnifiedVectorFormat> f(ncol);
	for (idx_t c = 0; c < ncol; c++) {
		Vector *src = &chunk.data[first + c];
		if (!strs[c] && src->GetType().id() != LogicalTypeId::BIGINT) {
			casts.emplace_back(LogicalType::BIGINT, n);
			src = AsBigint(ctx, *src, n, casts.back());
		}
		src->ToUnifiedFormat(f[c]);
	}
	std::string buf;
	for (idx_t r = 0; r < n; r++) {
		if (!keep[r]) {
			continue;
		}
		buf.clear();
		for (idx_t c = 0; c < ncol; c++) {
			const idx_t i = f[c].sel->get_index(r);
			if (nulls && (*nulls)[c]) {
				const char valid = f[c].validity.RowIsValid(i) ? 1 : 0;
				buf.push_back(valid);
				if (!valid) {
					continue;
				}
			}
			if (strs[c]) {
				const string_t &v = UnifiedVectorFormat::GetData<string_t>(f[c])[i];
				const uint32_t len = (uint32_t)v.GetSize();
				buf.append((const char *)&len, sizeof len);
				buf.append(v.GetData(), len);
			} else {
				const int64_t v = UnifiedVectorFormat::GetData<int64_t>(f[c])[i];
				buf.append((const char *)&v, sizeof v);
			}
		}
		dst.push_back(d.code(buf.data(), buf.size()));
	}
}

static void SinkCross(ExecutionContext &context, const TesseraPlanInfo &info, DataChunk &chunk, TesseraLocalSink &l) {
	const idx_t n = chunk.size();
	const idx_t nk = info.nk, nx = info.general_cross ? info.xcols.size() : 1,
	            ny = info.general_cross ? info.ycols.size() : 1;
	const idx_t x0 = 1 + nk, y0 = x0 + nx, v0 = y0 + ny;
	vector<bool> keep0(n, false), keep1(n, false);
	UnifiedVectorFormat sf;
	chunk.data[0].ToUnifiedFormat(sf);
	vector<UnifiedVectorFormat> cf(v0);
	for (idx_t c = 1; c < v0; c++) {
		chunk.data[c].ToUnifiedFormat(cf[c]);
	}
	auto valid = [&](idx_t c, idx_t r) {
		return cf[c].validity.RowIsValid(cf[c].sel->get_index(r));
	};
	auto side = UnifiedVectorFormat::GetData<int32_t>(sf);
	for (idx_t r = 0; r < n; r++) {
		bool key_ok = true;
		for (idx_t c = 1; c < x0; c++) {
			key_ok = key_ok && valid(c, r);
		}
		if (!key_ok) {
			continue;
		}
		const bool second = side[sf.sel->get_index(r)] != 0;
		bool group_ok = true;
		for (idx_t c = second ? y0 : x0; c < (second ? v0 : y0); c++) {
			group_ok = group_ok && (valid(c, r) || (info.general_cross && (second ? info.ycols[c - y0] : info.xcols[c - x0]).nullable));
		}
		if (!group_ok) {
			l.null_x = true;
			continue;
		}
		(second ? keep1 : keep0)[r] = true;
	}
	vector<bool> xstrs, ystrs, xnulls, ynulls;
	for (auto &c : info.xcols) {
		xstrs.push_back(c.str);
		xnulls.push_back(c.nullable);
	}
	for (auto &c : info.ycols) {
		ystrs.push_back(c.str);
		ynulls.push_back(c.nullable);
	}
	for (idx_t s = 0; s < 2; s++) {
		auto &keep = s ? keep1 : keep0;
		if (nk > 1) {
			AppendTupleCodes(context.client, chunk, 1, nk, info.k_strs, n, keep, l.kd, s ? l.k2 : l.k);
		} else if (info.k_str) {
			AppendCodes(chunk.data[1], n, keep, l.kd, s ? l.k2 : l.k);
		} else {
			AppendInt64(context.client, chunk.data[1], n, keep, s ? l.k2 : l.k);
		}
		const idx_t g0 = s ? y0 : x0, ng = s ? ny : nx;
		if (info.general_cross && TesseraPlanInfo::Tuples(s ? info.ycols : info.xcols)) {
			AppendTupleCodes(context.client, chunk, g0, ng, s ? ystrs : xstrs, n, keep, s ? l.yd : l.xd, s ? l.y : l.x,
			                 s ? &ynulls : &xnulls);
		} else if (s ? info.YDict() : info.XDict()) {
			AppendCodes(chunk.data[g0], n, keep, s ? l.yd : l.xd, s ? l.y : l.x);
		} else {
			AppendInt64(context.client, chunk.data[g0], n, keep, s ? l.y : l.x);
		}
		if (info.has_value && info.v_int) {
			AppendInt64V(context.client, chunk.data[v0 + s], n, keep, s ? l.iw : l.iv, l.bad_v);
		} else if (info.has_value) {
			AppendDouble(context.client, chunk.data[v0 + s], n, keep, s ? l.w : l.v, l.bad_v);
		}
	}
}

SinkResultType PhysicalTessera::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &l = input.local_state.Cast<TesseraLocalSink>();
	if (info.cross) {
		SinkCross(context, info, chunk, l);
		return SinkResultType::NEED_MORE_INPUT;
	}
	const idx_t n = chunk.size();
	vector<bool> keep(n, true);
	UnifiedVectorFormat kf, xf;
	chunk.data[info.k_pos].ToUnifiedFormat(kf);
	chunk.data[info.x_pos].ToUnifiedFormat(xf);
	for (idx_t r = 0; r < n; r++) {
		if (!kf.validity.RowIsValid(kf.sel->get_index(r))) {
			keep[r] = false;
		} else if (!xf.validity.RowIsValid(xf.sel->get_index(r))) {
			l.null_x = true;
			keep[r] = false;
		}
	}
	if (info.k_str) {
		AppendCodes(chunk.data[info.k_pos], n, keep, l.kd, l.k);
	} else {
		AppendInt64(context.client, chunk.data[info.k_pos], n, keep, l.k);
	}
	if (info.x_str) {
		AppendCodes(chunk.data[info.x_pos], n, keep, l.xd, l.x);
	} else {
		AppendInt64(context.client, chunk.data[info.x_pos], n, keep, l.x);
	}
	if (info.has_value && info.v_int) {
		AppendInt64V(context.client, chunk.data[info.v_pos], n, keep, l.iv, l.bad_v);
	} else if (info.has_value) {
		auto &vv = chunk.data[info.v_pos];
		Vector tmp(LogicalType::DOUBLE, n);
		Vector *src = &vv;
		if (vv.GetType().id() != LogicalTypeId::DOUBLE) {
			VectorOperations::Cast(context.client, vv, tmp, n);
			src = &tmp;
		}
		UnifiedVectorFormat vf;
		src->ToUnifiedFormat(vf);
		auto d = UnifiedVectorFormat::GetData<double>(vf);
		for (idx_t r = 0; r < n; r++) {
			if (!keep[r]) {
				continue;
			}
			auto idx = vf.sel->get_index(r);
			if (!vf.validity.RowIsValid(idx)) {
				l.bad_v = true;
			}
			l.v.push_back(d[idx]);
		}
	}
	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType PhysicalTessera::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
	auto &g = input.global_state.Cast<TesseraGlobalSink>();
	auto &l = input.local_state.Cast<TesseraLocalSink>();
	std::unique_ptr<tessera::QueryPart> part(new tessera::QueryPart());
	part->k = std::move(l.k);
	part->x = std::move(l.x);
	part->v = std::move(l.v);
	part->iv = std::move(l.iv);
	std::unique_ptr<tessera::QueryPart> part_y(new tessera::QueryPart());
	part_y->k = std::move(l.k2);
	part_y->x = std::move(l.y);
	part_y->v = std::move(l.w);
	part_y->iv = std::move(l.iw);
	vector<int64_t> kmap, xmap, ymap;
	{
		lock_guard<mutex> guard(g.lock);
		for (size_t c = 0; c < l.kd.size(); c++) {
			kmap.push_back(g.kd.code(l.kd.str(c).data(), l.kd.str(c).size()));
		}
		for (size_t c = 0; c < l.xd.size(); c++) {
			xmap.push_back(g.xd.code(l.xd.str(c).data(), l.xd.str(c).size()));
		}
		for (size_t c = 0; c < l.yd.size(); c++) {
			ymap.push_back(g.yd.code(l.yd.str(c).data(), l.yd.str(c).size()));
		}
	}
	if (info.KDict()) {
		for (auto &k : part->k) {
			k = kmap[k];
		}
		for (auto &k : part_y->k) {
			k = kmap[k];
		}
	}
	if (info.XDict()) {
		for (auto &x : part->x) {
			x = xmap[x];
		}
	}
	if (info.cross && info.YDict()) {
		for (auto &y : part_y->x) {
			y = ymap[y];
		}
	}
	lock_guard<mutex> guard(g.lock);
	g.null_x = g.null_x || l.null_x;
	g.bad_v = g.bad_v || l.bad_v;
	g.parts.push_back(std::move(part));
	if (info.cross) {
		g.parts_y.push_back(std::move(part_y));
	}
	return SinkCombineResultType::FINISHED;
}

struct PairFilterCtx {
	const TesseraPlanInfo *info;
	const tessera::StrDict *xd, *yd;
};
static bool PairPasses(void *ctx, int64_t a, int64_t b);

static bool IntegerRange(const LogicalType &t, __int128 &lo, __int128 &hi) {
	if (t.id() == LogicalTypeId::DECIMAL) {
		const uint8_t w = DecimalType::GetWidth(t);
		if (w >= 38) {
			return false;
		}
		hi = 1;
		for (uint8_t d = 0; d < w; d++) {
			hi *= 10;
		}
		hi -= 1;
		lo = -hi;
		return true;
	}
	int bits;
	bool sgn = true;
	switch (t.InternalType()) {
	case PhysicalType::INT8: bits = 8; break;
	case PhysicalType::INT16: bits = 16; break;
	case PhysicalType::INT32: bits = 32; break;
	case PhysicalType::INT64: bits = 64; break;
	case PhysicalType::UINT8: bits = 8; sgn = false; break;
	case PhysicalType::UINT16: bits = 16; sgn = false; break;
	case PhysicalType::UINT32: bits = 32; sgn = false; break;
	case PhysicalType::UINT64: bits = 64; sgn = false; break;
	default: return false;
	}
	hi = sgn ? ((__int128)1 << (bits - 1)) - 1 : ((__int128)1 << bits) - 1;
	lo = sgn ? -hi - 1 : 0;
	return true;
}

SinkFinalizeType PhysicalTessera::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                           OperatorSinkFinalizeInput &input) const {
	auto &g = input.global_state.Cast<TesseraGlobalSink>();
	if (g.null_x || g.bad_v) {
		throw InvalidInputException("tessera: a NULL %s appeared after the query was planned on a column without NULLs "
		                            "(a prepared statement); prepare it again, or SET tessera_enabled = false",
		                            g.null_x ? "group key" : "value");
	}
	if (info.XDict() && !info.cross && !tessera::both_orders(static_cast<tessera::PairMode>(info.mode))) {
		const vector<int64_t> perm = g.xd.finish();
		for (auto &p : g.parts) {
			for (auto &x : p->x) {
				x = perm[x];
			}
		}
	}
	tessera::QuerySpec q;
	q.has_value = info.has_value;
	q.distinct_keys = info.distinct_keys;
	q.mode = static_cast<tessera::PairMode>(info.mode);
	q.min_value = info.min_value;
	q.topk = info.topk;
	q.similarity = static_cast<tessera::Similarity>(info.sim_measure);
	q.min_sim = info.min_sim;
	q.threads = g.threads;
	q.verbose = getenv("TESSERA_VERBOSE") != nullptr;
	Value setting;
	if (context.TryGetCurrentSetting("tessera_frontier", setting) && !setting.IsNull()) {
		q.frontier = setting.GetValue<int64_t>();
	}
	if (context.TryGetCurrentSetting("tessera_reorder", setting) && !setting.IsNull()) {
		q.reorder = setting.GetValue<bool>();
	}
	q.should_stop = [](void *c) {
		return static_cast<ClientContext *>(c)->IsInterrupted();
	};
	q.stop_ctx = &context;
	PairFilterCtx pctx {&info, &g.xd, &g.yd};
	__int128 plo, phi;
	if (info.general_cross && !info.filters.empty() && info.v_int && IntegerRange(info.prod_type, plo, phi)) {
		q.pair_pass = PairPasses;
		q.pair_ctx = &pctx;
		q.prod_lo = plo;
		q.prod_hi = phi;
	}
	auto &bm = BufferManager::GetBufferManager(context);
	const idx_t max_mem = bm.GetMaxMemory(), used = bm.GetUsedMemory();
	q.out_budget = max_mem > used ? max_mem - used : 1;
	if (bm.HasTemporaryDirectory()) {
		q.spill_dir = bm.GetTemporaryDirectory();
		q.spill_mkdir = true;
		const optional_idx max_swap = bm.GetMaxSwap();
		if (max_swap.IsValid()) {
			const idx_t swap_used = bm.GetUsedSwap();
			q.spill_limit = max_swap.GetIndex() > swap_used ? max_swap.GetIndex() - swap_used : 1;
		}
	} else {
		q.spill = false;
	}
	tessera::QueryResult res;
	try {
		if (info.symmetric) {
			q.mode = tessera::PairMode::FULL;
			if (info.two) {
				tessera::run_query_two(g.parts, nullptr, q, res);
			} else {
				tessera::run_query(g.parts, q, res);
			}
		} else if (info.two) {
			tessera::run_query_two(g.parts, &g.parts_y, q, res);
		} else if (info.cross) {
			tessera::run_query_cross(g.parts, g.parts_y, q, res);
		} else {
			tessera::run_query(g.parts, q, res);
		}
	} catch (tessera::Cancelled &) {
		throw InterruptException();
	} catch (tessera::OutOfMemory &e) {
		throw OutOfMemoryException(e.what());
	} catch (std::bad_alloc &) {
		throw OutOfMemoryException("tessera: out of memory");
	} catch (tessera::SpillError &e) {
		if (e.limit) {
			throw OutOfMemoryException(e.what());
		}
		throw IOException(e.what());
	} catch (std::runtime_error &e) {
		throw NotImplementedException(e.what());
	}
	if (info.v_int && res.int_values) {
		__int128 lo, hi;
		if (IntegerRange(info.prod_type, lo, hi) && (res.prod_min < lo || res.prod_max > hi)) {
			throw OutOfRangeException("Overflow in multiplication of %s (tessera: a product of two joined rows)!",
			                          TypeIdToString(info.prod_type.InternalType()));
		}
		const LogicalType &st = info.two ? info.agg2_type : info.agg_type;
		if (st.id() != LogicalTypeId::DECIMAL && IntegerRange(st, lo, hi) && (res.cell_min < lo || res.cell_max > hi)) {
			throw OutOfRangeException("Overflow in SUM of %s (tessera: the sum of a group)!",
			                          TypeIdToString((info.two ? info.agg2_type : info.agg_type).InternalType()));
		}
	}
	g.orig = std::move(res.orig);
	g.out = std::move(res.out);
	g.general = res.general;
	g.wide = res.wide;
	g.side = std::move(res.side);
	g.out2 = std::move(res.out2);
	g.general2 = res.general2;
	return SinkFinalizeType::READY;
}

class TesseraGlobalSource : public GlobalSourceState {
public:
	explicit TesseraGlobalSource(TesseraGlobalSink &g, int threads) : threads(threads) {
		for (uint32_t p = 0; p < g.out.size(); p++) {
			for (size_t s = 0; s < tessera::cell_count(g.out[p], g.wide); s += BLOCK) {
				blocks.push_back({p, s});
			}
		}
	}
	static constexpr size_t BLOCK = STANDARD_VECTOR_SIZE / 2;
	vector<std::pair<uint32_t, size_t>> blocks;
	std::atomic<size_t> next {0};
	int threads;
	idx_t MaxThreads() override {
		return threads;
	}
};

unique_ptr<GlobalSourceState> PhysicalTessera::GetGlobalSourceState(ClientContext &context) const {
	auto &g = sink_state->Cast<TesseraGlobalSink>();
	return make_uniq<TesseraGlobalSource>(g, g.threads);
}

static inline double CellDouble(uint64_t s) {
	double d;
	memcpy(&d, &s, sizeof d);
	return d;
}
template <class F>
static inline F WideToFloat(__int128 v) {
	const uint64_t lo = (uint64_t)v;
	const int64_t hi = (int64_t)(v >> 64);
	return hi == -1 ? -F(UINT64_MAX - lo) - 1 : F(lo) + F(hi) * (F(UINT64_MAX) + 1);
}
static double AverageOf(const LogicalType &in, __int128 s, int64_t n, bool general2) {
	if (general2) {
		return CellDouble((uint64_t)s) / (double)(uint64_t)n;
	}
	double scale = 1;
	if (in.id() == LogicalTypeId::DECIMAL) {
		__int128 p = 1;
		for (uint8_t d = 0; d < DecimalType::GetScale(in); d++) {
			p *= 10;
		}
		scale = WideToFloat<double>(p);
	}
	if (in.InternalType() == PhysicalType::INT16) {
		double divident = double((uint64_t)n);
		if (scale != 1) {
			divident *= scale;
		}
		return double((int64_t)s) / divident;
	}
	long double divident = (long double)(uint64_t)n;
	if (scale != 1) {
		divident *= scale;
	}
	return (double)(WideToFloat<long double>(s) / divident);
}
template <class A, bool GEN>
struct CellValue;
template <bool GEN>
struct CellValue<int64_t, GEN> {
	static int64_t Get(uint64_t s) {
		return GEN ? (int64_t)std::llround(CellDouble(s)) : (int64_t)s;
	}
};
template <bool GEN>
struct CellValue<hugeint_t, GEN> {
	static hugeint_t Get(uint64_t s) {
		return hugeint_t(CellValue<int64_t, GEN>::Get(s));
	}
};
template <bool GEN>
struct CellValue<double, GEN> {
	static double Get(uint64_t s) {
		return GEN ? CellDouble(s) : (double)(int64_t)s;
	}
};
template <class A>
struct WideCell;
template <>
struct WideCell<hugeint_t> {
	static hugeint_t Get(const tessera::Triple *c, size_t q) {
		return hugeint_t((int64_t)c[2 * q + 1].s, c[2 * q].s);
	}
};
template <>
struct WideCell<int64_t> {
	static int64_t Get(const tessera::Triple *c, size_t q) {
		return (int64_t)c[2 * q].s;
	}
};
template <>
struct WideCell<double> {
	static double Get(const tessera::Triple *c, size_t q) {
		return (double)(__int128)(((unsigned __int128)c[2 * q + 1].s << 64) | c[2 * q].s);
	}
};
template <class AT, bool GEN, bool WIDE>
static inline AT AggOf(const tessera::Triple *cells, size_t q) {
	return WIDE ? WideCell<AT>::Get(cells, q) : CellValue<AT, GEN>::Get(cells[q].s);
}

template <class XT, class AT, bool GEN, bool WIDE = false>
static idx_t EmitRows(const tessera::Triple *cells, size_t s0, size_t s1, const int64_t *orig, const TesseraPlanInfo &info,
                      XT *x0, XT *x1, AT *ag) {
	const int64_t o0 = info.x_off[0], o1 = info.x_off[1];
	const TesseraPairMode mode = info.mode;
	idx_t row = 0;
	for (size_t q = s0; q < s1; q++) {
		const tessera::Triple &t = cells[WIDE ? 2 * q : q];
		const int64_t oi = orig[t.i], oj = orig[t.j];
		const AT v = ag ? AggOf<AT, GEN, WIDE>(cells, q) : AT();
		if (t.i == t.j) {
			if (mode == TesseraPairMode::LT || mode == TesseraPairMode::NE) {
				continue;
			}
			x0[row] = (XT)(oi - o0);
			x1[row] = (XT)(oi - o1);
			if (ag) ag[row] = v;
			row++;
		} else if (mode == TesseraPairMode::FULL || mode == TesseraPairMode::NE) {
			x0[row] = (XT)(oi - o0);
			x1[row] = (XT)(oj - o1);
			if (ag) ag[row] = v;
			row++;
			x0[row] = (XT)(oj - o0);
			x1[row] = (XT)(oi - o1);
			if (ag) ag[row] = v;
			row++;
		} else {
			const int64_t lo = std::min(oi, oj), hi = std::max(oi, oj);
			x0[row] = (XT)((info.col0_smaller ? lo : hi) - o0);
			x1[row] = (XT)((info.col0_smaller ? hi : lo) - o1);
			if (ag) ag[row] = v;
			row++;
		}
	}
	return row;
}

template <class XT>
static idx_t EmitRowsX(const tessera::Triple *cells, size_t s0, size_t s1, const int64_t *orig,
                       const TesseraPlanInfo &info, bool general, bool wide, DataChunk &chunk) {
	XT *x0 = FlatVector::GetDataMutable<XT>(chunk.data[0]);
	XT *x1 = FlatVector::GetDataMutable<XT>(chunk.data[1]);
	if (chunk.ColumnCount() == 2) {
		return EmitRows<XT, int64_t, false>(cells, s0, s1, orig, info, x0, x1, (int64_t *)nullptr);
	}
	auto &a = chunk.data[2];
	if (wide) {
		switch (a.GetType().InternalType()) {
		case PhysicalType::INT64:
			return EmitRows<XT, int64_t, false, true>(cells, s0, s1, orig, info, x0, x1,
			                                          FlatVector::GetDataMutable<int64_t>(a));
		case PhysicalType::INT128:
			return EmitRows<XT, hugeint_t, false, true>(cells, s0, s1, orig, info, x0, x1,
			                                            FlatVector::GetDataMutable<hugeint_t>(a));
		case PhysicalType::DOUBLE:
			return EmitRows<XT, double, false, true>(cells, s0, s1, orig, info, x0, x1,
			                                         FlatVector::GetDataMutable<double>(a));
		default:
			throw InternalException("tessera: unsupported aggregate type");
		}
	}
	switch (a.GetType().InternalType()) {
	case PhysicalType::INT64:
		return general ? EmitRows<XT, int64_t, true>(cells, s0, s1, orig, info, x0, x1,
		                                             FlatVector::GetDataMutable<int64_t>(a))
		               : EmitRows<XT, int64_t, false>(cells, s0, s1, orig, info, x0, x1,
		                                              FlatVector::GetDataMutable<int64_t>(a));
	case PhysicalType::INT128:
		return general ? EmitRows<XT, hugeint_t, true>(cells, s0, s1, orig, info, x0, x1,
		                                               FlatVector::GetDataMutable<hugeint_t>(a))
		               : EmitRows<XT, hugeint_t, false>(cells, s0, s1, orig, info, x0, x1,
		                                                FlatVector::GetDataMutable<hugeint_t>(a));
	case PhysicalType::DOUBLE:
		return general ? EmitRows<XT, double, true>(cells, s0, s1, orig, info, x0, x1,
		                                            FlatVector::GetDataMutable<double>(a))
		               : EmitRows<XT, double, false>(cells, s0, s1, orig, info, x0, x1,
		                                             FlatVector::GetDataMutable<double>(a));
	default:
		throw InternalException("tessera: unsupported aggregate type");
	}
}

static idx_t EmitStrings(ClientContext &ctx, const tessera::Triple *cells, size_t s0, size_t s1, const int64_t *orig,
                         const TesseraPlanInfo &info, bool general, bool wide, const tessera::StrDict &dict,
                         DataChunk &chunk) {
	DataChunk codes;
	vector<LogicalType> ct {LogicalType::BIGINT, LogicalType::BIGINT};
	if (chunk.ColumnCount() == 3) {
		ct.push_back(chunk.data[2].GetType());
	}
	codes.Initialize(Allocator::Get(ctx), ct);
	TesseraPlanInfo plain = info;
	plain.x_off[0] = plain.x_off[1] = 0;
	const idx_t row = EmitRowsX<int64_t>(cells, s0, s1, orig, plain, general, wide, codes);
	if (chunk.ColumnCount() == 3) {
		chunk.data[2].Reference(codes.data[2]);
	}
	for (idx_t g = 0; g < 2; g++) {
		const int64_t *c = FlatVector::GetData<int64_t>(codes.data[g]);
		Vector strs(LogicalType::VARCHAR, STANDARD_VECTOR_SIZE);
		auto sp = FlatVector::GetDataMutable<string_t>(strs);
		for (idx_t r = 0; r < row; r++) {
			const tessera::StrDict::Bytes s = dict.str(c[r]);
			sp[r] = StringVector::AddString(strs, s.data(), s.size());
		}
		if (!info.x_compress[g]) {
			chunk.data[g].Reference(strs);
			continue;
		}
		DataChunk in;
		in.InitializeEmpty({LogicalType::VARCHAR});
		in.data[0].Reference(strs);
		in.SetChildCardinality(row);
		ExpressionExecutor ex(ctx, *info.x_compress[g]);
		ex.ExecuteExpression(in, chunk.data[g]);
	}
	return row;
}

static void WriteGroup(ClientContext &ctx, Vector &out, const int64_t *val, idx_t row, bool str, const tessera::StrDict &dict,
                       int64_t off, const shared_ptr<Expression> &compress) {
	if (str) {
		Vector strs(LogicalType::VARCHAR, STANDARD_VECTOR_SIZE);
		auto sp = FlatVector::GetDataMutable<string_t>(strs);
		for (idx_t r = 0; r < row; r++) {
			const tessera::StrDict::Bytes s = dict.str(val[r]);
			sp[r] = StringVector::AddString(strs, s.data(), s.size());
		}
		if (!compress) {
			out.Reference(strs);
			return;
		}
		DataChunk in;
		in.InitializeEmpty({LogicalType::VARCHAR});
		in.data[0].Reference(strs);
		in.SetChildCardinality(row);
		ExpressionExecutor ex(ctx, *compress);
		ex.ExecuteExpression(in, out);
		return;
	}
#define TESSERA_WRITE(T)                                                                                               \
	{                                                                                                                  \
		auto d = FlatVector::GetDataMutable<T>(out);                                                                   \
		for (idx_t r = 0; r < row; r++) {                                                                              \
			d[r] = (T)(val[r] - off);                                                                                  \
		}                                                                                                              \
		return;                                                                                                        \
	}
	switch (out.GetType().InternalType()) {
	case PhysicalType::INT64: TESSERA_WRITE(int64_t)
	case PhysicalType::INT32: TESSERA_WRITE(int32_t)
	case PhysicalType::INT16: TESSERA_WRITE(int16_t)
	case PhysicalType::INT8: TESSERA_WRITE(int8_t)
	case PhysicalType::UINT32: TESSERA_WRITE(uint32_t)
	case PhysicalType::UINT16: TESSERA_WRITE(uint16_t)
	case PhysicalType::UINT8: TESSERA_WRITE(uint8_t)
	default:
		throw InternalException("tessera: unsupported key type");
	}
#undef TESSERA_WRITE
}

template <class AT, bool GEN, bool WIDE = false>
static void WriteAgg(const tessera::Triple *cells, size_t s0, size_t s1, AT *ag) {
	for (size_t q = s0; q < s1; q++) {
		ag[q - s0] = AggOf<AT, GEN, WIDE>(cells, q);
	}
}

static idx_t EmitCross(ClientContext &ctx, const tessera::Triple *cells, size_t s0, size_t s1, const int64_t *orig,
                       const uint8_t *side, const TesseraPlanInfo &info, bool general, bool wide,
                       const tessera::StrDict &xd, const tessera::StrDict &yd, DataChunk &chunk) {
	int64_t xv[STANDARD_VECTOR_SIZE], yv[STANDARD_VECTOR_SIZE];
	const idx_t row = s1 - s0;
	for (size_t q = s0; q < s1; q++) {
		const tessera::Triple &t = cells[wide ? 2 * q : q];
		const bool sw = side[t.i] != 0;
		xv[q - s0] = orig[sw ? t.j : t.i];
		yv[q - s0] = orig[sw ? t.i : t.j];
	}
	WriteGroup(ctx, chunk.data[0], xv, row, info.x_str, xd, info.x_off[0], info.x_compress[0]);
	WriteGroup(ctx, chunk.data[1], yv, row, info.y_str, yd, info.x_off[1], info.x_compress[1]);
	auto &a = chunk.data[2];
	if (wide) {
		switch (a.GetType().InternalType()) {
		case PhysicalType::INT64:
			WriteAgg<int64_t, false, true>(cells, s0, s1, FlatVector::GetDataMutable<int64_t>(a));
			break;
		case PhysicalType::INT128:
			WriteAgg<hugeint_t, false, true>(cells, s0, s1, FlatVector::GetDataMutable<hugeint_t>(a));
			break;
		case PhysicalType::DOUBLE:
			WriteAgg<double, false, true>(cells, s0, s1, FlatVector::GetDataMutable<double>(a));
			break;
		default: throw InternalException("tessera: unsupported aggregate type");
		}
		return row;
	}
	switch (a.GetType().InternalType()) {
	case PhysicalType::INT64:
		general ? WriteAgg<int64_t, true>(cells, s0, s1, FlatVector::GetDataMutable<int64_t>(a))
		        : WriteAgg<int64_t, false>(cells, s0, s1, FlatVector::GetDataMutable<int64_t>(a));
		break;
	case PhysicalType::INT128:
		general ? WriteAgg<hugeint_t, true>(cells, s0, s1, FlatVector::GetDataMutable<hugeint_t>(a))
		        : WriteAgg<hugeint_t, false>(cells, s0, s1, FlatVector::GetDataMutable<hugeint_t>(a));
		break;
	case PhysicalType::DOUBLE:
		general ? WriteAgg<double, true>(cells, s0, s1, FlatVector::GetDataMutable<double>(a))
		        : WriteAgg<double, false>(cells, s0, s1, FlatVector::GetDataMutable<double>(a));
		break;
	default:
		throw InternalException("tessera: unsupported aggregate type");
	}
	return row;
}

template <class AT, bool GEN, bool WIDE = false>
static void WriteAggAt(const tessera::Triple *cells, const vector<size_t> &qs, AT *ag) {
	for (idx_t r = 0; r < qs.size(); r++) {
		ag[r] = AggOf<AT, GEN, WIDE>(cells, qs[r]);
	}
}

struct ItemVal {
	int64_t i = 0;
	const char *s = nullptr;
	uint32_t len = 0;
	bool null = false;
};

static void DecodeItem(int64_t v, bool dict, const vector<TesseraPlanInfo::TupleCol> &cols, const tessera::StrDict &d,
                       ItemVal *out) {
	if (!dict) {
		out[0].i = v;
		return;
	}
	const tessera::StrDict::Bytes b = d.str(v);
	if (!TesseraPlanInfo::Tuples(cols)) {
		out[0].s = b.data();
		out[0].len = (uint32_t)b.size();
		return;
	}
	size_t p = 0;
	for (idx_t c = 0; c < cols.size(); c++) {
		out[c].null = false;
		if (cols[c].nullable && b[p++] == 0) {
			out[c].null = true;
			continue;
		}
		if (cols[c].str) {
			memcpy(&out[c].len, b.data() + p, sizeof(uint32_t));
			p += sizeof(uint32_t);
			out[c].s = b.data() + p;
			p += out[c].len;
		} else {
			memcpy(&out[c].i, b.data() + p, sizeof(int64_t));
			p += sizeof(int64_t);
		}
	}
}

static bool PassOne(const ItemVal &a, const ItemVal &b, bool str, ExpressionType op) {
	if (a.null || b.null) {
		return op == ExpressionType::COMPARE_DISTINCT_FROM && a.null != b.null;
	}
	int cmp;
	if (str) {
		const int m = memcmp(a.s, b.s, std::min(a.len, b.len));
		cmp = m != 0 ? m : (a.len < b.len ? -1 : a.len > b.len ? 1 : 0);
	} else {
		cmp = a.i < b.i ? -1 : a.i > b.i ? 1 : 0;
	}
	switch (op) {
	case ExpressionType::COMPARE_LESSTHAN: return cmp < 0;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO: return cmp <= 0;
	case ExpressionType::COMPARE_GREATERTHAN: return cmp > 0;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO: return cmp >= 0;
	default: return cmp != 0;
	}
}

static bool PassFilters(const TesseraPlanInfo &info, const ItemVal *xv, const ItemVal *yv) {
	for (auto &f : info.filters) {
		if (!PassOne(xv[f.xi], yv[f.yi], info.xcols[f.xi].str, f.op)) {
			return false;
		}
	}
	return true;
}

static bool PairPasses(void *ctx, int64_t a, int64_t b) {
	auto &p = *static_cast<PairFilterCtx *>(ctx);
	const auto &info = *p.info;
	const bool xdict = info.XDict(), ydict = info.symmetric ? xdict : info.YDict();
	const tessera::StrDict &yds = info.symmetric ? *p.xd : *p.yd;
	vector<ItemVal> xv(info.xcols.size()), yv(info.ycols.size());
	for (int o = 0; o < (info.symmetric ? 2 : 1); o++) {
		DecodeItem(o ? b : a, xdict, info.xcols, *p.xd, xv.data());
		DecodeItem(o ? a : b, ydict, info.ycols, yds, yv.data());
		if (PassFilters(info, xv.data(), yv.data())) {
			return true;
		}
	}
	return false;
}

static idx_t EmitCrossGeneral(ClientContext &ctx, const tessera::Triple *cells, size_t s0, size_t s1, const int64_t *orig,
                              const uint8_t *side, const TesseraPlanInfo &info, bool general, bool wide,
                              const tessera::StrDict &xd, const tessera::StrDict &yd, DataChunk &chunk,
                              const __int128 *sums, bool general2) {
	using Val = ItemVal;
	const idx_t nx = info.xcols.size(), ny = info.ycols.size();
	const bool xdict = info.XDict(), ydict = info.symmetric ? xdict : info.YDict();
	auto decode = DecodeItem;
	vector<Val> xs(2 * (s1 - s0) * nx), ys(2 * (s1 - s0) * ny);
	vector<size_t> qs;
	const tessera::StrDict &yds = info.symmetric ? xd : yd;
	for (size_t q = s0; q < s1; q++) {
		const tessera::Triple &t = cells[wide ? 2 * q : q];
		const bool sw = !info.symmetric && side[t.i] != 0;
		for (int o = 0; o < (info.symmetric && t.i != t.j ? 2 : 1); o++) {
			const bool flip = sw != (o == 1);
			Val *xv = &xs[qs.size() * nx], *yv = &ys[qs.size() * ny];
			decode(orig[flip ? t.j : t.i], xdict, info.xcols, xd, xv);
			decode(orig[flip ? t.i : t.j], ydict, info.ycols, yds, yv);
			bool ok = true;
			for (auto &f : info.filters) {
				ok = ok && PassOne(xv[f.xi], yv[f.yi], info.xcols[f.xi].str, f.op);
			}
			if (ok) {
				qs.push_back(q);
			}
		}
	}
	const idx_t row = qs.size();
	for (idx_t g = 0; g < info.out_groups.size(); g++) {
		const bool second = info.out_groups[g].first != 0;
		const idx_t c = info.out_groups[g].second, nc = second ? ny : nx;
		const auto &col = (second ? info.ycols : info.xcols)[c];
		const Val *vals = second ? ys.data() : xs.data();
		Vector &out = chunk.data[g];
		if (col.nullable) {
			for (idx_t r = 0; r < row; r++) {
				if (vals[r * nc + c].null) {
					FlatVector::SetNull(out, r, true);
				}
			}
		}
		if (col.str) {
			Vector strs(LogicalType::VARCHAR, STANDARD_VECTOR_SIZE);
			auto sp = FlatVector::GetDataMutable<string_t>(strs);
			for (idx_t r = 0; r < row; r++) {
				if (vals[r * nc + c].null) {
					FlatVector::SetNull(strs, r, true);
					continue;
				}
				sp[r] = StringVector::AddString(strs, vals[r * nc + c].s, vals[r * nc + c].len);
			}
			if (!col.compress) {
				out.Reference(strs);
				continue;
			}
			DataChunk in;
			in.InitializeEmpty({LogicalType::VARCHAR});
			in.data[0].Reference(strs);
			in.SetChildCardinality(row);
			ExpressionExecutor ex(ctx, *col.compress);
			ex.ExecuteExpression(in, out);
			continue;
		}
#define TESSERA_WRITE(T)                                                                                               \
	{                                                                                                                  \
		auto d = FlatVector::GetDataMutable<T>(out);                                                                   \
		for (idx_t r = 0; r < row; r++) {                                                                              \
			d[r] = (T)(vals[r * nc + c].i - col.off);                                                                  \
		}                                                                                                              \
		break;                                                                                                         \
	}
		switch (out.GetType().InternalType()) {
		case PhysicalType::INT64: TESSERA_WRITE(int64_t)
		case PhysicalType::INT32: TESSERA_WRITE(int32_t)
		case PhysicalType::INT16: TESSERA_WRITE(int16_t)
		case PhysicalType::INT8: TESSERA_WRITE(int8_t)
		case PhysicalType::UINT32: TESSERA_WRITE(uint32_t)
		case PhysicalType::UINT16: TESSERA_WRITE(uint16_t)
		case PhysicalType::UINT8: TESSERA_WRITE(uint8_t)
		default:
			throw InternalException("tessera: unsupported key type");
		}
#undef TESSERA_WRITE
	}
	auto write_sums = [&](Vector &sv) {
		for (idx_t r = 0; r < row; r++) {
			const __int128 v = sums[qs[r]];
			switch (sv.GetType().InternalType()) {
			case PhysicalType::INT64:
				FlatVector::GetDataMutable<int64_t>(sv)[r] = (int64_t)v;
				break;
			case PhysicalType::INT128:
				FlatVector::GetDataMutable<hugeint_t>(sv)[r] = hugeint_t((int64_t)(v >> 64), (uint64_t)v);
				break;
			default: {
				double d;
				if (general2) {
					const uint64_t bits = (uint64_t)v;
					memcpy(&d, &bits, sizeof d);
				} else {
					d = (double)v;
				}
				FlatVector::GetDataMutable<double>(sv)[r] = d;
			}
			}
		}
	};
	if (!info.aggs.empty()) {
		for (idx_t i = 0; i < info.aggs.size(); i++) {
			Vector &out = chunk.data[info.out_groups.size() + i];
			if (info.aggs[i] == 1) {
				write_sums(out);
				continue;
			}
			for (idx_t r = 0; r < row; r++) {
				const size_t q = qs[r];
				const int64_t n = wide      ? (int64_t)cells[2 * q].s
				                  : general ? (int64_t)std::llround(CellDouble(cells[q].s))
				                            : (int64_t)cells[q].s;
				if (info.aggs[i] == 0) {
					FlatVector::GetDataMutable<int64_t>(out)[r] = n;
				} else {
					FlatVector::GetDataMutable<double>(out)[r] = AverageOf(info.avg_input, sums[q], n, general2);
				}
			}
		}
		return row;
	}
	if (info.two) {
		write_sums(chunk.data[info.out_groups.size() + (info.count_first ? 1 : 0)]);
	}
	auto &a = chunk.data[info.out_groups.size() + (info.two && !info.count_first ? 1 : 0)];
	switch (a.GetType().InternalType()) {
	case PhysicalType::INT64:
		wide      ? WriteAggAt<int64_t, false, true>(cells, qs, FlatVector::GetDataMutable<int64_t>(a))
		: general ? WriteAggAt<int64_t, true>(cells, qs, FlatVector::GetDataMutable<int64_t>(a))
		          : WriteAggAt<int64_t, false>(cells, qs, FlatVector::GetDataMutable<int64_t>(a));
		break;
	case PhysicalType::INT128:
		wide      ? WriteAggAt<hugeint_t, false, true>(cells, qs, FlatVector::GetDataMutable<hugeint_t>(a))
		: general ? WriteAggAt<hugeint_t, true>(cells, qs, FlatVector::GetDataMutable<hugeint_t>(a))
		          : WriteAggAt<hugeint_t, false>(cells, qs, FlatVector::GetDataMutable<hugeint_t>(a));
		break;
	case PhysicalType::DOUBLE:
		wide      ? WriteAggAt<double, false, true>(cells, qs, FlatVector::GetDataMutable<double>(a))
		: general ? WriteAggAt<double, true>(cells, qs, FlatVector::GetDataMutable<double>(a))
		          : WriteAggAt<double, false>(cells, qs, FlatVector::GetDataMutable<double>(a));
		break;
	default:
		throw InternalException("tessera: unsupported aggregate type");
	}
	return row;
}

SourceResultType PhysicalTessera::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                  OperatorSourceInput &input) const {
	auto &g = sink_state->Cast<TesseraGlobalSink>();
	auto &src = input.global_state.Cast<TesseraGlobalSource>();
	size_t b = src.next.fetch_add(1);
	if (b >= src.blocks.size()) {
		chunk.SetChildCardinality(0);
		return SourceResultType::FINISHED;
	}
	auto &cells = g.out[src.blocks[b].first];
	size_t s0 = src.blocks[b].second, s1 = std::min(tessera::cell_count(cells, g.wide), s0 + TesseraGlobalSource::BLOCK);
	const tessera::Triple *c = cells.data.get();
	const int64_t *orig = g.orig.data();
	idx_t row;
	if (info.cross) {
		row = info.general_cross
		          ? EmitCrossGeneral(context.client, c, s0, s1, orig, g.side.data(), info, g.general, g.wide, g.xd, g.yd, chunk,
		                             info.two ? g.out2[src.blocks[b].first].data() : nullptr, g.general2)
		          : EmitCross(context.client, c, s0, s1, orig, g.side.data(), info, g.general, g.wide, g.xd, g.yd, chunk);
		chunk.SetChildCardinality(row);
		return SourceResultType::HAVE_MORE_OUTPUT;
	}
	if (chunk.data[0].GetType().InternalType() != chunk.data[1].GetType().InternalType()) {
		throw InternalException("tessera: group columns of different physical types");
	}
	if (info.x_str) {
		row = EmitStrings(context.client, c, s0, s1, orig, info, g.general, g.wide, g.xd, chunk);
		chunk.SetChildCardinality(row);
		return SourceResultType::HAVE_MORE_OUTPUT;
	}
	switch (chunk.data[0].GetType().InternalType()) {
	case PhysicalType::INT64:
		row = EmitRowsX<int64_t>(c, s0, s1, orig, info, g.general, g.wide, chunk);
		break;
	case PhysicalType::INT32:
		row = EmitRowsX<int32_t>(c, s0, s1, orig, info, g.general, g.wide, chunk);
		break;
	case PhysicalType::INT16:
		row = EmitRowsX<int16_t>(c, s0, s1, orig, info, g.general, g.wide, chunk);
		break;
	case PhysicalType::INT8:
		row = EmitRowsX<int8_t>(c, s0, s1, orig, info, g.general, g.wide, chunk);
		break;
	case PhysicalType::UINT32:
		row = EmitRowsX<uint32_t>(c, s0, s1, orig, info, g.general, g.wide, chunk);
		break;
	case PhysicalType::UINT16:
		row = EmitRowsX<uint16_t>(c, s0, s1, orig, info, g.general, g.wide, chunk);
		break;
	case PhysicalType::UINT8:
		row = EmitRowsX<uint8_t>(c, s0, s1, orig, info, g.general, g.wide, chunk);
		break;
	default:
		throw InternalException("tessera: unsupported key type");
	}
	chunk.SetChildCardinality(row);
	return SourceResultType::HAVE_MORE_OUTPUT;
}

}
