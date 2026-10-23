#define DUCKDB_EXTENSION_MAIN
#include "tessera_extension.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension_util.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_distinct.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_set_operation.hpp"
#include "duckdb/planner/operator/logical_top_n.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "tessera_operator.hpp"
#include "tessera.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace duckdb {

static idx_t BaseColumn(LogicalGet &get, const ColumnBinding &b) {
	if (b.table_index != get.table_index) {
		return DConstants::INVALID_INDEX;
	}
	auto &ids = get.GetColumnIds();
	if (b.column_index >= ids.size()) {
		return DConstants::INVALID_INDEX;
	}
	return ids[b.column_index].GetPrimaryIndex();
}

static bool IsStringKey(const LogicalType &t) {
	return t.id() == LogicalTypeId::VARCHAR && StringType::GetCollation(t).empty();
}

static bool IsIntegerKey(const LogicalType &t) {
	switch (t.id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
		return true;
	default:
		return false;
	}
}

struct SidePair {
	ColumnBinding l, r;
};

static bool SideOf(Expression &e, LogicalGet &gl, LogicalGet &gr, int &side, idx_t &base, ColumnBinding &b) {
	if (e.expression_class != ExpressionClass::BOUND_COLUMN_REF) {
		return false;
	}
	b = e.Cast<BoundColumnRefExpression>().binding;
	base = BaseColumn(gl, b);
	if (base != DConstants::INVALID_INDEX) {
		side = 0;
		return true;
	}
	base = BaseColumn(gr, b);
	if (base != DConstants::INVALID_INDEX) {
		side = 1;
		return true;
	}
	return false;
}

static bool SameColumnBothSides(Expression &a, Expression &b, LogicalGet &gl, LogicalGet &gr, SidePair &out,
                                ColumnBinding *a_binding = nullptr) {
	int sa, sb;
	idx_t ca, cb;
	ColumnBinding ba, bb;
	if (!SideOf(a, gl, gr, sa, ca, ba) || !SideOf(b, gl, gr, sb, cb, bb) || sa == sb || ca != cb) {
		return false;
	}
	out.l = sa == 0 ? ba : bb;
	out.r = sa == 0 ? bb : ba;
	if (a_binding) {
		*a_binding = ba;
	}
	return true;
}

static Expression *ThroughProjections(Expression &e, const vector<LogicalProjection *> &projs, int64_t *offset,
                                      shared_ptr<Expression> *str_compress = nullptr) {
	Expression *cur = &e;
	for (auto *p : projs) {
		if (cur->expression_class != ExpressionClass::BOUND_COLUMN_REF) {
			return nullptr;
		}
		auto &b = cur->Cast<BoundColumnRefExpression>().binding;
		if (b.table_index != p->table_index || b.column_index >= p->expressions.size()) {
			return nullptr;
		}
		cur = p->expressions[b.column_index].get();
		if (cur->expression_class == ExpressionClass::BOUND_FUNCTION &&
		    StringUtil::StartsWith(cur->Cast<BoundFunctionExpression>().function.name, "__internal_compress_string_")) {
			auto &f = cur->Cast<BoundFunctionExpression>();
			if (!str_compress || *str_compress || f.children.size() != 1) {
				return nullptr;
			}
			auto c = f.Copy();
			auto &cf = c->Cast<BoundFunctionExpression>();
			cf.children[0] = make_uniq<BoundReferenceExpression>(f.children[0]->return_type, 0);
			*str_compress = shared_ptr<Expression>(c.release());
			cur = f.children[0].get();
		} else if (cur->expression_class == ExpressionClass::BOUND_FUNCTION) {
			auto &f = cur->Cast<BoundFunctionExpression>();
			const bool comp = StringUtil::StartsWith(f.function.name, "__internal_compress_integral_");
			const bool decomp = StringUtil::StartsWith(f.function.name, "__internal_decompress_integral_");
			if (!offset || !(comp || decomp) || f.children.size() != 2 ||
			    f.children[1]->expression_class != ExpressionClass::BOUND_CONSTANT) {
				return nullptr;
			}
			const int64_t min = f.children[1]->Cast<BoundConstantExpression>().value.GetValue<int64_t>();
			*offset += comp ? min : -min;
			cur = f.children[0].get();
		}
	}
	return cur->expression_class == ExpressionClass::BOUND_COLUMN_REF ? cur : nullptr;
}

struct ScanSide {
	vector<LogicalProjection *> projs;
	LogicalGet *get = nullptr;
};

static bool ScanBelow(LogicalOperator &child, ScanSide &s) {
	LogicalOperator *cur = &child;
	while (cur->type == LogicalOperatorType::LOGICAL_PROJECTION && cur->children.size() == 1) {
		s.projs.push_back(&cur->Cast<LogicalProjection>());
		cur = cur->children[0].get();
	}
	if (cur->type != LogicalOperatorType::LOGICAL_GET) {
		return false;
	}
	s.get = &cur->Cast<LogicalGet>();
	return true;
}

static Expression *ToScan(Expression *e, ScanSide &l, ScanSide &r, bool key, int64_t *off = nullptr) {
	if (!e || e->expression_class != ExpressionClass::BOUND_COLUMN_REF) {
		return e;
	}
	auto &b = e->Cast<BoundColumnRefExpression>().binding;
	for (auto *s : {&l, &r}) {
		if (!s->projs.empty() && b.table_index == s->projs[0]->table_index) {
			int64_t offset = 0;
			shared_ptr<Expression> str_compress;
			return key ? ThroughProjections(*e, s->projs, &offset, &str_compress) : ThroughProjections(*e, s->projs, off);
		}
	}
	return e;
}

static unique_ptr<LogicalOperator> TakeScan(unique_ptr<LogicalOperator> &child) {
	unique_ptr<LogicalOperator> *cur = &child;
	while ((*cur)->type == LogicalOperatorType::LOGICAL_PROJECTION) {
		cur = &(*cur)->children[0];
	}
	return std::move(*cur);
}

static bool NoNulls(ClientContext &context, LogicalGet &get, idx_t base_column) {
	if (!get.function.statistics) {
		return false;
	}
	auto stats = get.function.statistics(context, get.bind_data.get(), base_column);
	return stats && !stats->CanHaveNull();
}

struct TesseraPushdown {
	uint64_t min_value = 0;
	int64_t topk = -1;
};

static bool TryTessera(ClientContext &context, unique_ptr<LogicalOperator> &plan, const TesseraPushdown &push);

static unique_ptr<LogicalOperator> *AggregateBelow(LogicalOperator &op, ColumnBinding b, bool &is_value) {
	is_value = false;
	unique_ptr<LogicalOperator> *slot = &op.children[0];
	bool track = true;
	while ((*slot)->type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto &proj = (*slot)->Cast<LogicalProjection>();
		if (track) {
			if (b.table_index != proj.table_index || b.column_index >= proj.expressions.size() ||
			    proj.expressions[b.column_index]->expression_class != ExpressionClass::BOUND_COLUMN_REF) {
				track = false;
			} else {
				b = proj.expressions[b.column_index]->Cast<BoundColumnRefExpression>().binding;
			}
		}
		slot = &(*slot)->children[0];
	}
	if ((*slot)->type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		return nullptr;
	}
	auto &agg = (*slot)->Cast<LogicalAggregate>();
	is_value = track && b.table_index == agg.aggregate_index && b.column_index == 0;
	return slot;
}

static unique_ptr<LogicalOperator> *HavingBelow(LogicalFilter &filter, TesseraPushdown &push) {
	unique_ptr<LogicalOperator> *slot = nullptr;
	for (auto &e : filter.expressions) {
		if (e->expression_class != ExpressionClass::BOUND_COMPARISON) {
			continue;
		}
		auto &cmp = e->Cast<BoundComparisonExpression>();
		Expression *col = cmp.left.get(), *cst = cmp.right.get();
		ExpressionType t = cmp.type;
		if (col->expression_class != ExpressionClass::BOUND_COLUMN_REF) {
			std::swap(col, cst);
			t = FlipComparisonExpression(t);
		}
		if (col->expression_class != ExpressionClass::BOUND_COLUMN_REF ||
		    cst->expression_class != ExpressionClass::BOUND_CONSTANT) {
			continue;
		}
		bool is_value;
		auto s = AggregateBelow(filter, col->Cast<BoundColumnRefExpression>().binding, is_value);
		if (!s || !is_value) {
			continue;
		}
		auto &v = cst->Cast<BoundConstantExpression>().value;
		if (v.IsNull() || !v.type().IsNumeric()) {
			continue;
		}
		if (t != ExpressionType::COMPARE_GREATERTHANOREQUALTO && t != ExpressionType::COMPARE_GREATERTHAN) {
			continue;
		}
		const double lo = std::floor(v.GetValue<double>());
		if (lo >= 1 && lo < 9e18) {
			push.min_value = std::max<uint64_t>(push.min_value, (uint64_t)lo);
			slot = s;
		}
	}
	return slot;
}

static unique_ptr<LogicalOperator> *TopNBelow(LogicalTopN &top, TesseraPushdown &push) {
	if (top.offset != 0 || top.orders.empty() || top.orders[0].type != OrderType::DESCENDING ||
	    top.orders[0].expression->expression_class != ExpressionClass::BOUND_COLUMN_REF) {
		return nullptr;
	}
	bool is_value;
	auto s = AggregateBelow(top, top.orders[0].expression->Cast<BoundColumnRefExpression>().binding, is_value);
	if (!s || !is_value || top.limit == 0 || top.limit > (1ull << 40)) {
		return nullptr;
	}
	push.topk = (int64_t)top.limit;
	return s;
}

static constexpr double MIN_EXPANSION = 4.0;
static constexpr double MIN_EXPANSION_CROSS = 4.0;
static double Expansion(ClientContext &context, LogicalGet &ga, idx_t ka, LogicalGet *gb, idx_t kb) {
	auto rows = [&](LogicalGet &g) -> double {
		if (!g.function.cardinality) {
			return -1;
		}
		auto st = g.function.cardinality(context, g.bind_data.get());
		return st && st->has_estimated_cardinality ? (double)st->estimated_cardinality : -1;
	};
	auto distinct = [&](LogicalGet &g, idx_t c) -> double {
		if (c == DConstants::INVALID_INDEX || !g.function.statistics) {
			return -1;
		}
		auto st = g.function.statistics(context, g.bind_data.get(), c);
		return st && st->GetDistinctCount() > 0 ? (double)st->GetDistinctCount() : -1;
	};
	const double na = rows(ga), va = distinct(ga, ka);
	if (na < 0 || va < 0) {
		return -1;
	}
	if (!gb) {
		return na / va;
	}
	const double nb = rows(*gb), vb = distinct(*gb, kb);
	if (nb < 0 || vb < 0 || na + nb <= 0) {
		return -1;
	}
	return na * nb / (std::max(va, vb) * (na + nb));
}

static bool JoinExpands(ClientContext &context, double expansion, bool cross) {
	Value v;
	double theta = cross ? MIN_EXPANSION_CROSS : MIN_EXPANSION;
	if (context.TryGetCurrentSetting(cross ? "tessera_min_expansion_cross" : "tessera_min_expansion", v) && !v.IsNull()) {
		theta = v.GetValue<double>();
	}
	const bool run = theta <= 0 || expansion < 0 || expansion >= theta;
	if (getenv("TESSERA_VERBOSE")) {
		fprintf(stderr, "tessera: %.3g joined rows per input row (estimate), limit %.3g: %s\n", expansion, theta,
		        run ? "Tessera" : "the engine's plan");
	}
	return run;
}

static bool TryTessera(ClientContext &context, unique_ptr<LogicalOperator> &plan, const TesseraPushdown &push) {
	auto &agg = plan->Cast<LogicalAggregate>();
	if (agg.groups.size() != 2 || agg.expressions.size() > 1 || agg.grouping_sets.size() > 1 ||
	    !agg.grouping_functions.empty()) {
		return false;
	}
	vector<LogicalProjection *> projs;
	unique_ptr<LogicalOperator> *slot = &plan->children[0];
	while ((*slot)->type == LogicalOperatorType::LOGICAL_PROJECTION) {
		projs.push_back(&(*slot)->Cast<LogicalProjection>());
		slot = &(*slot)->children[0];
	}
	if ((*slot)->type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		return false;
	}
	auto &join = (*slot)->Cast<LogicalComparisonJoin>();
	ScanSide scan_l, scan_r;
	if (join.join_type != JoinType::INNER || join.predicate || !join.duplicate_eliminated_columns.empty() ||
	    join.children.size() != 2 || !ScanBelow(*join.children[0], scan_l) || !ScanBelow(*join.children[1], scan_r)) {
		return false;
	}
	auto &gl = *scan_l.get;
	auto &gr = *scan_r.get;
	auto tl = gl.GetTable(), tr = gr.GetTable();
	if (!tl || !tr || tl.get() != tr.get() || !TableFilterSet::Equals(&gl.table_filters, &gr.table_filters)) {
		return false;
	}
	if (gl.extra_info.sample_options || gr.extra_info.sample_options) {
		return false;
	}
	TesseraPlanInfo info;
	auto g0e = ToScan(ThroughProjections(*agg.groups[0], projs, &info.x_off[0], &info.x_compress[0]), scan_l, scan_r, false,
	                  &info.x_off[0]);
	auto g1e = ToScan(ThroughProjections(*agg.groups[1], projs, &info.x_off[1], &info.x_compress[1]), scan_l, scan_r, false,
	                  &info.x_off[1]);
	SidePair x;
	if (!g0e || !g1e || !SameColumnBothSides(*g0e, *g1e, gl, gr, x)) {
		return false;
	}
	ColumnBinding g0 = g0e->Cast<BoundColumnRefExpression>().binding;
	info.x_bind = x.l;
	info.x_type = agg.groups[0]->return_type;
	info.x_str = IsStringKey(g0e->return_type);
	if (info.x_str ? !(info.x_compress[0] || info.x_type == g0e->return_type) || info.x_off[0] || info.x_off[1] ||
	                     (bool)info.x_compress[0] != (bool)info.x_compress[1]
	               : !IsIntegerKey(info.x_type) || !IsIntegerKey(g0e->return_type) || info.x_compress[0] || info.x_compress[1]) {
		return false;
	}
	if (agg.expressions.empty()) {
		info.distinct_only = true;
		info.has_value = false;
		if (push.min_value > 0 || push.topk > 0) {
			return false;
		}
	} else if (agg.expressions[0]->expression_class != ExpressionClass::BOUND_AGGREGATE) {
		return false;
	}
	if (!info.distinct_only) {
	auto &ae = agg.expressions[0]->Cast<BoundAggregateExpression>();
	if (ae.IsDistinct() || ae.filter || ae.order_bys) {
		return false;
	}
	info.agg_type = ae.return_type;
	if (ae.function.name == "count_star" && ae.children.empty()) {
		info.has_value = false;
	} else if (ae.function.name == "sum" && ae.children.size() == 1 &&
	           ae.children[0]->expression_class == ExpressionClass::BOUND_FUNCTION) {
		auto &mul = ae.children[0]->Cast<BoundFunctionExpression>();
		if (mul.function.name != "*" || mul.children.size() != 2) {
			return false;
		}
		int64_t voff[2] = {0, 0};
		auto v0 = ToScan(ThroughProjections(*mul.children[0], projs, &voff[0]), scan_l, scan_r, false, &voff[0]);
		auto v1 = ToScan(ThroughProjections(*mul.children[1], projs, &voff[1]), scan_l, scan_r, false, &voff[1]);
		if (voff[0] != 0 || voff[1] != 0) {
			return false;
		}
		SidePair v;
		if (!v0 || !v1 || !SameColumnBothSides(*v0, *v1, gl, gr, v)) {
			return false;
		}
		info.has_value = true;
		info.v_bind = v.l;
		info.v_type = v0->return_type;
		if (!IsIntegerKey(info.v_type) && info.v_type.id() != LogicalTypeId::DOUBLE &&
		    info.v_type.id() != LogicalTypeId::FLOAT) {
			return false;
		}
	} else {
		return false;
	}
	auto at = info.agg_type.InternalType();
	if (at != PhysicalType::INT64 && at != PhysicalType::INT128 && at != PhysicalType::DOUBLE) {
		return false;
	}
	}
	bool have_eq = false, have_cmp = false;
	for (auto &cond : join.conditions) {
		if (cond.comparison == ExpressionType::COMPARE_EQUAL && !have_eq) {
			SidePair k;
			auto kl = ToScan(cond.left.get(), scan_l, scan_r, true), kr = ToScan(cond.right.get(), scan_l, scan_r, true);
			if (!kl || !kr || !SameColumnBothSides(*kl, *kr, gl, gr, k)) {
				return false;
			}
			info.k_bind = k.l;
			info.k_str = IsStringKey(kl->return_type);
			if (!info.k_str && !IsIntegerKey(kl->return_type)) {
				return false;
			}
			have_eq = true;
			continue;
		}
		bool lt = cond.comparison == ExpressionType::COMPARE_LESSTHAN ||
		          cond.comparison == ExpressionType::COMPARE_LESSTHANOREQUALTO;
		bool gt = cond.comparison == ExpressionType::COMPARE_GREATERTHAN ||
		          cond.comparison == ExpressionType::COMPARE_GREATERTHANOREQUALTO;
		if ((lt || gt) && !have_cmp) {
			SidePair c;
			ColumnBinding left_b;
			auto cl = ToScan(cond.left.get(), scan_l, scan_r, true), cr = ToScan(cond.right.get(), scan_l, scan_r, true);
			if (!cl || !cr || !SameColumnBothSides(*cl, *cr, gl, gr, c, &left_b) || !(c.l == x.l)) {
				return false;
			}
			ColumnBinding u = left_b;
			if (gt) {
				u = (left_b == c.l) ? c.r : c.l;
			}
			bool eq = cond.comparison == ExpressionType::COMPARE_LESSTHANOREQUALTO ||
			          cond.comparison == ExpressionType::COMPARE_GREATERTHANOREQUALTO;
			info.mode = eq ? TesseraPairMode::LE : TesseraPairMode::LT;
			info.col0_smaller = (u == g0);
			have_cmp = true;
			continue;
		}
		return false;
	}
	if (!have_eq || info.k_bind == info.x_bind || (info.has_value && (info.v_bind == info.k_bind))) {
		return false;
	}
	if (!NoNulls(context, gl, BaseColumn(gl, info.x_bind)) ||
	    (info.has_value && !NoNulls(context, gl, BaseColumn(gl, info.v_bind)))) {
		return false;
	}
	if (!JoinExpands(context, Expansion(context, gl, BaseColumn(gl, info.k_bind), nullptr, 0), false)) {
		return false;
	}
	auto join_owned = std::move(*slot);
	auto agg_owned = unique_ptr_cast<LogicalOperator, LogicalAggregate>(std::move(plan));
	agg_owned->children.clear();
	auto child = TakeScan(join_owned->children[0]);
	info.min_value = push.min_value;
	info.topk = push.topk;
	plan = make_uniq<LogicalTessera>(std::move(agg_owned), std::move(child), std::move(info));
	plan->ResolveOperatorTypes();
	return true;
}

static bool TryTesseraCross(ClientContext &context, unique_ptr<LogicalOperator> &plan, const TesseraPushdown &push,
                            Binder &binder) {
	auto &agg = plan->Cast<LogicalAggregate>();
	if (agg.groups.size() != 2 || agg.expressions.size() != 1 || agg.grouping_sets.size() > 1 ||
	    !agg.grouping_functions.empty() || agg.expressions[0]->expression_class != ExpressionClass::BOUND_AGGREGATE) {
		return false;
	}
	vector<LogicalProjection *> projs;
	unique_ptr<LogicalOperator> *slot = &plan->children[0];
	while ((*slot)->type == LogicalOperatorType::LOGICAL_PROJECTION) {
		projs.push_back(&(*slot)->Cast<LogicalProjection>());
		slot = &(*slot)->children[0];
	}
	if ((*slot)->type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		return false;
	}
	auto &join = (*slot)->Cast<LogicalComparisonJoin>();
	ScanSide scan_l, scan_r;
	if (join.join_type != JoinType::INNER || join.predicate || !join.duplicate_eliminated_columns.empty() ||
	    join.children.size() != 2 || !ScanBelow(*join.children[0], scan_l) || !ScanBelow(*join.children[1], scan_r) ||
	    join.conditions.size() != 1 || join.conditions[0].comparison != ExpressionType::COMPARE_EQUAL) {
		return false;
	}
	auto &gl = *scan_l.get;
	auto &gr = *scan_r.get;
	if (!gl.GetTable() || !gr.GetTable()) {
		return false;
	}
	TesseraPlanInfo info;
	info.cross = true;
	Expression *ge[2];
	int gs[2];
	idx_t gc[2];
	ColumnBinding gb[2];
	for (idx_t g = 0; g < 2; g++) {
		ge[g] = ToScan(ThroughProjections(*agg.groups[g], projs, &info.x_off[g], &info.x_compress[g]), scan_l, scan_r, false,
		               &info.x_off[g]);
		if (!ge[g] || !SideOf(*ge[g], gl, gr, gs[g], gc[g], gb[g])) {
			return false;
		}
		const bool str = IsStringKey(ge[g]->return_type);
		if (str ? info.x_off[g] != 0 || (!info.x_compress[g] && agg.groups[g]->return_type != ge[g]->return_type)
		        : !IsIntegerKey(ge[g]->return_type) || !IsIntegerKey(agg.groups[g]->return_type) || info.x_compress[g]) {
			return false;
		}
	}
	if (gs[0] == gs[1] || (gl.GetTable().get() == gr.GetTable().get() && gc[0] == gc[1])) {
		return false;
	}
	info.x_type = agg.groups[0]->return_type;
	info.y_type = agg.groups[1]->return_type;
	info.x_str = IsStringKey(ge[0]->return_type);
	info.y_str = IsStringKey(ge[1]->return_type);
	const int sx = gs[0];
	LogicalGet &gx = sx == 0 ? gl : gr, &gy = sx == 0 ? gr : gl;
	auto &cond = join.conditions[0];
	auto kle = ToScan(cond.left.get(), scan_l, scan_r, true), kre = ToScan(cond.right.get(), scan_l, scan_r, true);
	int sl, sr;
	idx_t cl, cr;
	ColumnBinding bl, br;
	if (!kle || !kre || !SideOf(*kle, gl, gr, sl, cl, bl) || !SideOf(*kre, gl, gr, sr, cr, br) || sl == sr) {
		return false;
	}
	const ColumnBinding kx = sl == sx ? bl : br, ky = sl == sx ? br : bl;
	const LogicalType kxt = (sl == sx ? kle : kre)->return_type;
	const LogicalType kyt = (sl == sx ? kre : kle)->return_type;
	info.k_str = IsStringKey(kxt);
	if (!(IsIntegerKey(kxt) && IsIntegerKey(kyt)) && !(info.k_str && IsStringKey(kyt))) {
		return false;
	}
	auto &ae = agg.expressions[0]->Cast<BoundAggregateExpression>();
	if (ae.IsDistinct() || ae.filter || ae.order_bys) {
		return false;
	}
	info.agg_type = ae.return_type;
	ColumnBinding vb[2];
	LogicalType vt[2];
	if (ae.function.name == "count_star" && ae.children.empty()) {
		info.has_value = false;
	} else if (ae.function.name == "sum" && ae.children.size() == 1 &&
	           ae.children[0]->expression_class == ExpressionClass::BOUND_FUNCTION) {
		auto &mul = ae.children[0]->Cast<BoundFunctionExpression>();
		if (mul.function.name != "*" || mul.children.size() != 2) {
			return false;
		}
		bool seen[2] = {false, false};
		for (idx_t f = 0; f < 2; f++) {
			int64_t voff = 0;
			auto ve = ToScan(ThroughProjections(*mul.children[f], projs, &voff), scan_l, scan_r, false, &voff);
			if (voff != 0) {
				return false;
			}
			int s;
			idx_t c;
			ColumnBinding b;
			if (!ve || !SideOf(*ve, gl, gr, s, c, b) || seen[s == sx ? 0 : 1]) {
				return false;
			}
			const idx_t side = s == sx ? 0 : 1;
			seen[side] = true;
			vb[side] = b;
			vt[side] = ve->return_type;
			if (!IsIntegerKey(vt[side]) && vt[side].id() != LogicalTypeId::DOUBLE && vt[side].id() != LogicalTypeId::FLOAT) {
				return false;
			}
		}
		info.has_value = true;
	} else {
		return false;
	}
	auto at = info.agg_type.InternalType();
	if (at != PhysicalType::INT64 && at != PhysicalType::INT128 && at != PhysicalType::DOUBLE) {
		return false;
	}
	if (!NoNulls(context, gx, BaseColumn(gx, gb[0])) || !NoNulls(context, gy, BaseColumn(gy, gb[1])) ||
	    (info.has_value && (!NoNulls(context, gx, BaseColumn(gx, vb[0])) || !NoNulls(context, gy, BaseColumn(gy, vb[1]))))) {
		return false;
	}
	if (!JoinExpands(context, Expansion(context, gx, BaseColumn(gx, kx), &gy, BaseColumn(gy, ky)), true)) {
		return false;
	}
	const LogicalType kt = info.k_str ? LogicalType::VARCHAR : LogicalType::BIGINT;
	const LogicalType xt = ge[0]->return_type, yt = ge[1]->return_type;
	auto colref = [](const LogicalType &t, ColumnBinding b) -> unique_ptr<Expression> {
		return make_uniq<BoundColumnRefExpression>(t, b);
	};
	auto side_rows = [&](unique_ptr<LogicalOperator> get, idx_t side) -> unique_ptr<LogicalOperator> {
		vector<unique_ptr<Expression>> e;
		e.push_back(make_uniq<BoundConstantExpression>(Value::INTEGER((int32_t)side)));
		e.push_back(BoundCastExpression::AddCastToType(context, colref(side ? kyt : kxt, side ? ky : kx), kt));
		e.push_back(side ? unique_ptr<Expression>(make_uniq<BoundConstantExpression>(Value(xt))) : colref(xt, gb[0]));
		e.push_back(side ? colref(yt, gb[1]) : unique_ptr<Expression>(make_uniq<BoundConstantExpression>(Value(yt))));
		if (info.has_value) {
			for (idx_t s = 0; s < 2; s++) {
				e.push_back(s == side ? BoundCastExpression::AddCastToType(context, colref(vt[s], vb[s]), LogicalType::DOUBLE)
				                      : unique_ptr<Expression>(make_uniq<BoundConstantExpression>(Value(LogicalType::DOUBLE))));
			}
		}
		auto proj = make_uniq<LogicalProjection>(binder.GenerateTableIndex(), std::move(e));
		proj->children.push_back(std::move(get));
		return std::move(proj);
	};
	auto join_owned = std::move(*slot);
	auto agg_owned = unique_ptr_cast<LogicalOperator, LogicalAggregate>(std::move(plan));
	agg_owned->children.clear();
	auto first = TakeScan(join_owned->children[sx]);
	auto second = TakeScan(join_owned->children[1 - sx]);
	const idx_t ncols = info.has_value ? 6 : 4;
	auto all = make_uniq<LogicalSetOperation>(binder.GenerateTableIndex(), ncols, side_rows(std::move(first), 0),
	                                          side_rows(std::move(second), 1), LogicalOperatorType::LOGICAL_UNION, true);
	all->ResolveOperatorTypes();
	info.min_value = push.min_value;
	info.topk = push.topk;
	plan = make_uniq<LogicalTessera>(std::move(agg_owned), std::move(all), std::move(info));
	plan->ResolveOperatorTypes();
	return true;
}

static bool TryTesseraDistinct(ClientContext &context, unique_ptr<LogicalOperator> &plan) {
	auto &d = plan->Cast<LogicalDistinct>();
	if (d.distinct_type != DistinctType::DISTINCT || d.order_by || d.children.size() != 1 ||
	    d.children[0]->type != LogicalOperatorType::LOGICAL_PROJECTION) {
		return false;
	}
	auto &proj = d.children[0]->Cast<LogicalProjection>();
	if (proj.expressions.size() != 2 || d.distinct_targets.size() != 2) {
		return false;
	}
	bool seen[2] = {false, false};
	for (auto &t : d.distinct_targets) {
		if (t->expression_class != ExpressionClass::BOUND_COLUMN_REF) {
			return false;
		}
		auto &b = t->Cast<BoundColumnRefExpression>().binding;
		if (b.table_index != proj.table_index || b.column_index > 1 || seen[b.column_index]) {
			return false;
		}
		seen[b.column_index] = true;
	}
	vector<unique_ptr<Expression>> none;
	auto agg = make_uniq<LogicalAggregate>(proj.table_index, DConstants::INVALID_INDEX, std::move(none));
	for (idx_t c = 0; c < 2; c++) {
		agg->groups.push_back(
		    make_uniq<BoundColumnRefExpression>(proj.expressions[c]->return_type, ColumnBinding(proj.table_index, c)));
	}
	agg->children.push_back(std::move(d.children[0]));
	unique_ptr<LogicalOperator> ap = std::move(agg);
	if (TryTessera(context, ap, TesseraPushdown())) {
		plan = std::move(ap);
		return true;
	}
	d.children[0] = std::move(ap->children[0]);
	return false;
}

static void Inject(ClientContext &context, unique_ptr<LogicalOperator> &plan, Binder &binder) {
	if (!plan) {
		return;
	}
	if (plan->type == LogicalOperatorType::LOGICAL_DISTINCT && TryTesseraDistinct(context, plan)) {
		return;
	}
	if (plan->type == LogicalOperatorType::LOGICAL_FILTER || plan->type == LogicalOperatorType::LOGICAL_TOP_N) {
		TesseraPushdown push;
		auto slot = plan->type == LogicalOperatorType::LOGICAL_FILTER ? HavingBelow(plan->Cast<LogicalFilter>(), push)
		                                                              : TopNBelow(plan->Cast<LogicalTopN>(), push);
		if (slot && (TryTessera(context, *slot, push) || TryTesseraCross(context, *slot, push, binder))) {
			return;
		}
	}
	if (plan->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY &&
	    (TryTessera(context, plan, TesseraPushdown()) || TryTesseraCross(context, plan, TesseraPushdown(), binder))) {
		return;
	}
	for (auto &child : plan->children) {
		Inject(context, child, binder);
	}
}

static unique_ptr<LogicalOperator> TesseraSimBind(ClientContext &context, TableFunctionBindInput &input, idx_t bind_index,
                                                  vector<string> &names) {
	const char *what[] = {"the table", "k", "x", "the measure", "the threshold"};
	for (idx_t a = 0; a < 5; a++) {
		if (input.inputs[a].IsNull()) {
			throw BinderException("tessera_sim: %s is NULL", what[a]);
		}
	}
	const string tbl = input.inputs[0].GetValue<string>(), kc = input.inputs[1].GetValue<string>(),
	             xc = input.inputs[2].GetValue<string>(), ms = input.inputs[3].GetValue<string>();
	const double min_sim = input.inputs[4].GetValue<double>();
	const int measure = ms == "cosine" ? 1 : ms == "jaccard" ? 2 : ms == "dice" ? 3 : ms == "overlap" ? 4 : 0;
	if (!measure) {
		throw BinderException("tessera_sim: measure is 'cosine', 'jaccard', 'dice' or 'overlap'");
	}
	string vc, md = "lt";
	for (auto &kv : input.named_parameters) {
		if (kv.first == "v" && !kv.second.IsNull()) {
			vc = kv.second.GetValue<string>();
		} else if (kv.first == "mode" && !kv.second.IsNull()) {
			md = kv.second.GetValue<string>();
		}
	}
	TesseraPlanInfo info;
	if (md == "lt") {
		info.mode = TesseraPairMode::LT;
	} else if (md == "le") {
		info.mode = TesseraPairMode::LE;
	} else if (md == "full") {
		info.mode = TesseraPairMode::FULL;
	} else {
		throw BinderException("tessera_sim: mode is 'lt', 'le' or 'full'");
	}
	auto quote = [](const string &id) { return KeywordHelper::WriteOptionallyQuoted(id); };
	auto qn = QualifiedName::Parse(tbl);
	string from = (qn.catalog.empty() ? "" : quote(qn.catalog) + ".") + (qn.schema.empty() ? "" : quote(qn.schema) + ".") +
	              quote(qn.name);
	string sql = "SELECT " + quote(kc) + ", " + quote(xc) + (vc.empty() ? "" : ", " + quote(vc)) + " FROM " + from;
	Parser parser(context.GetParserOptions());
	parser.ParseQuery(sql);
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw BinderException("tessera_sim: cannot read %s", tbl);
	}
	auto child_binder = Binder::CreateBinder(context, input.binder);
	auto bound = child_binder->Bind(*parser.statements[0]->Cast<SelectStatement>().node);
	auto child = std::move(bound.plan);
	child->ResolveOperatorTypes();
	auto cb = child->GetColumnBindings();
	const LogicalType kt = bound.types[0], xt = bound.types[1];
	info.k_str = IsStringKey(kt);
	info.x_str = IsStringKey(xt);
	if (!info.k_str && !IsIntegerKey(kt)) {
		throw BinderException("tessera_sim: k must be an integer or VARCHAR column");
	}
	if (!info.x_str && !IsIntegerKey(xt)) {
		throw BinderException("tessera_sim: x must be an integer or VARCHAR column");
	}
	info.x_type = xt;
	info.k_bind = cb[0];
	info.x_bind = cb[1];
	if (!vc.empty()) {
		const LogicalType vt = bound.types[2];
		if (!IsIntegerKey(vt) && vt.id() != LogicalTypeId::DOUBLE && vt.id() != LogicalTypeId::FLOAT) {
			throw BinderException("tessera_sim: v must be an integer, FLOAT or DOUBLE column");
		}
		info.has_value = true;
		info.v_type = vt;
		info.v_bind = cb[2];
	}
	info.col0_smaller = true;
	info.sim_measure = measure;
	info.min_sim = min_sim;
	info.agg_type = LogicalType::DOUBLE;
	auto &b = *input.binder;
	vector<unique_ptr<Expression>> none;
	auto agg = make_uniq<LogicalAggregate>(b.GenerateTableIndex(), b.GenerateTableIndex(), std::move(none));
	agg->groups.push_back(make_uniq<BoundColumnRefExpression>(xt, cb[1]));
	agg->groups.push_back(make_uniq<BoundColumnRefExpression>(xt, cb[1]));
	agg->expressions.push_back(make_uniq<BoundConstantExpression>(Value(LogicalType::DOUBLE)));
	const idx_t gi = agg->group_index, ai = agg->aggregate_index;
	auto op = make_uniq<LogicalTessera>(std::move(agg), std::move(child), std::move(info));
	for (idx_t c = 0; c < cb.size(); c++) {
		op->expressions.push_back(make_uniq<BoundColumnRefExpression>(bound.types[c], cb[c]));
	}
	vector<unique_ptr<Expression>> out;
	out.push_back(make_uniq<BoundColumnRefExpression>(xt, ColumnBinding(gi, 0)));
	out.push_back(make_uniq<BoundColumnRefExpression>(xt, ColumnBinding(gi, 1)));
	out.push_back(make_uniq<BoundColumnRefExpression>(LogicalType::DOUBLE, ColumnBinding(ai, 0)));
	auto proj = make_uniq<LogicalProjection>(bind_index, std::move(out));
	proj->children.push_back(std::move(op));
	names = {"x0", "x1", "s"};
	return std::move(proj);
}

static void TesseraOptimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	Value enabled;
	if (input.context.TryGetCurrentSetting("tessera_enabled", enabled) &&
	    !enabled.IsNull() && !enabled.GetValue<bool>()) {
		return;
	}
	Inject(input.context, plan, input.optimizer.binder);
}

void TesseraExtension::Load(DuckDB &db) {
	auto &config = DBConfig::GetConfig(*db.instance);
	OptimizerExtension ext;
	ext.optimize_function = TesseraOptimize;
	config.optimizer_extensions.push_back(std::move(ext));
	config.AddExtensionOption("tessera_enabled", "Replace self-join aggregates by the Tessera operator",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(true));
	config.AddExtensionOption("tessera_frontier",
	                          "Tessera dense frontier: items < H on the matrix engine (-1: cost model)",
	                          LogicalType::BIGINT, Value::BIGINT(-1));
	config.AddExtensionOption("tessera_reorder", "Tessera: shingle order of the rows for the dense grid",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(true));
	config.AddExtensionOption("tessera_min_expansion",
	                          "Tessera: the engine's plan keeps self-joins with fewer estimated joined rows per input row (0: never)",
	                          LogicalType::DOUBLE, Value::DOUBLE(MIN_EXPANSION));
	config.AddExtensionOption("tessera_min_expansion_cross",
	                          "Tessera: the same limit for products of two relations (0: never)", LogicalType::DOUBLE,
	                          Value::DOUBLE(MIN_EXPANSION_CROSS));
	TableFunction sim("tessera_sim",
	                  {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                   LogicalType::DOUBLE},
	                  nullptr, nullptr);
	sim.bind_operator = TesseraSimBind;
	sim.named_parameters["v"] = LogicalType::VARCHAR;
	sim.named_parameters["mode"] = LogicalType::VARCHAR;
	ExtensionUtil::RegisterFunction(*db.instance, sim);
}

std::string TesseraExtension::Name() {
	return "tessera";
}

std::string TesseraExtension::Version() const {
	return tessera::kVersion;
}

}

extern "C" {
DUCKDB_EXTENSION_API void tessera_init(duckdb::DatabaseInstance &db) {
	duckdb::DuckDB db_wrapper(db);
	db_wrapper.LoadExtension<duckdb::TesseraExtension>();
}
DUCKDB_EXTENSION_API const char *tessera_version() {
	return duckdb::DuckDB::LibraryVersion();
}
}
