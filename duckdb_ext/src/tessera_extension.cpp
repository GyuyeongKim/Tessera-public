#define DUCKDB_EXTENSION_MAIN
#include "tessera_extension.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/expression_iterator.hpp"
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
	if (e.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return false;
	}
	b = e.Cast<BoundColumnRefExpression>().Binding();
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

struct Layer {
	TableIndex index;
	const vector<unique_ptr<Expression>> *exprs;
};

static Layer LayerOf(LogicalProjection &p) {
	return {p.table_index, &p.expressions};
}

static Expression *ThroughProjections(Expression &e, const vector<Layer> &layers, int64_t *offset,
                                      shared_ptr<Expression> *str_compress = nullptr) {
	Expression *cur = &e;
	idx_t str_decompressed = 0;
	for (auto &layer : layers) {
		if (cur->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return nullptr;
		}
		auto &b = cur->Cast<BoundColumnRefExpression>().Binding();
		if (b.table_index != layer.index || b.column_index >= layer.exprs->size()) {
			return nullptr;
		}
		cur = (*layer.exprs)[b.column_index].get();
		if (cur->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
			continue;
		}
		auto &f = cur->Cast<BoundFunctionExpression>();
		auto &name = f.Function().GetName();
		if (name == "__internal_compressed_materialization_cast" && f.GetChildren().size() == 1 &&
		    IsIntegerKey(f.GetReturnType()) && IsIntegerKey(f.GetChildren()[0]->GetReturnType())) {
			cur = f.GetChildren()[0].get();
		} else if (name == "__internal_decompress_string" && f.GetChildren().size() == 1) {
			str_decompressed++;
			cur = f.GetChildren()[0].get();
		} else if (name.StartsWith("__internal_compress_string_") && str_decompressed > 0 &&
		           f.GetChildren().size() == 1) {
			str_decompressed--;
			cur = f.GetChildren()[0].get();
		} else if (name.StartsWith("__internal_compress_string_")) {
			if (!str_compress || *str_compress || f.GetChildren().size() != 1) {
				return nullptr;
			}
			auto c = f.Copy();
			auto &cf = c->Cast<BoundFunctionExpression>();
			cf.GetChildrenMutable()[0] = make_uniq<BoundReferenceExpression>(f.GetChildren()[0]->GetReturnType(), 0);
			*str_compress = shared_ptr<Expression>(c.release());
			cur = f.GetChildren()[0].get();
		} else {
			const bool comp = name.StartsWith("__internal_compress_integral_");
			const bool decomp = name.StartsWith("__internal_decompress_integral_");
			if (!offset || !(comp || decomp) || f.GetChildren().size() != 2 ||
			    f.GetChildren()[1]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
				return nullptr;
			}
			const int64_t min = f.GetChildren()[1]->Cast<BoundConstantExpression>().GetValue().GetValue<int64_t>();
			*offset += comp ? min : -min;
			cur = f.GetChildren()[0].get();
		}
	}
	return cur->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF && str_decompressed == 0 ? cur : nullptr;
}

static bool Step(const Expression *&cur, const Layer &layer) {
	if (cur->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return false;
	}
	auto &b = cur->Cast<BoundColumnRefExpression>().Binding();
	if (b.table_index != layer.index || b.column_index >= layer.exprs->size()) {
		return false;
	}
	cur = (*layer.exprs)[b.column_index].get();
	return true;
}

static Expression *Defined(Expression &e, const vector<Layer> &layers, vector<Layer> &rest) {
	const Expression *cur = &e;
	idx_t i = 0;
	while (i < layers.size() && Step(cur, layers[i])) {
		i++;
	}
	rest.assign(layers.begin() + i, layers.end());
	return const_cast<Expression *>(cur);
}

struct ScanSide {
	vector<Layer> layers;
	LogicalAggregate *eager = nullptr;
	LogicalGet *get = nullptr;
};

static bool ScanBelow(LogicalOperator &child, ScanSide &s) {
	LogicalOperator *cur = &child;
	while (cur->children.size() == 1) {
		if (cur->type == LogicalOperatorType::LOGICAL_PROJECTION) {
			s.layers.push_back(LayerOf(cur->Cast<LogicalProjection>()));
		} else if (cur->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY && !s.eager) {
			s.eager = &cur->Cast<LogicalAggregate>();
			s.layers.push_back({s.eager->group_index, &s.eager->groups});
		} else {
			break;
		}
		cur = cur->children[0].get();
	}
	if (cur->type != LogicalOperatorType::LOGICAL_GET) {
		return false;
	}
	s.get = &cur->Cast<LogicalGet>();
	return true;
}

static Expression *ToScan(Expression *e, ScanSide &l, ScanSide &r, bool key, int64_t *off = nullptr) {
	if (!e || e->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return e;
	}
	auto &b = e->Cast<BoundColumnRefExpression>().Binding();
	for (auto *s : {&l, &r}) {
		if (!s->layers.empty() && b.table_index == s->layers[0].index) {
			int64_t offset = 0;
			shared_ptr<Expression> str_compress;
			return key ? ThroughProjections(*e, s->layers, &offset, &str_compress)
			           : ThroughProjections(*e, s->layers, off);
		}
	}
	return e;
}

static bool IsCountStar(const Expression &e, AggregateStateExportMode mode) {
	if (e.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		return false;
	}
	auto &a = e.Cast<BoundAggregateExpression>();
	return a.Function().GetName() == "count_star" && a.GetChildren().empty() && !a.IsDistinct() && !a.GetFilter() &&
	       !a.GetOrderBys() && a.StateExportMode() == mode;
}

static bool EagerCount(const BoundAggregateExpression &ae, const vector<Layer> &above, const ScanSide &l,
                       const ScanSide &r) {
	if (!l.eager || !r.eager || ae.Function().GetName() != "combine_aggr" || ae.GetChildren().size() != 2 ||
	    ae.IsDistinct() || ae.GetFilter() || ae.GetOrderBys()) {
		return false;
	}
	for (auto *s : {&l, &r}) {
		if (s->eager->grouping_sets.size() > 1 || !s->eager->grouping_functions.empty()) {
			return false;
		}
	}
	auto output = [&](const Expression &arg, int &side) -> const Expression * {
		const Expression *cur = &arg;
		for (auto &layer : above) {
			if (!Step(cur, layer)) {
				return nullptr;
			}
		}
		for (side = 0; side < 2; side++) {
			auto &s = side == 0 ? l : r;
			const Expression *c = cur;
			for (auto &layer : s.layers) {
				if (layer.exprs == &s.eager->groups) {
					if (c->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
						auto &b = c->Cast<BoundColumnRefExpression>().Binding();
						if (b.table_index == s.eager->aggregate_index && b.column_index < s.eager->expressions.size()) {
							return s.eager->expressions[b.column_index].get();
						}
					}
					break;
				}
				if (!Step(c, layer)) {
					break;
				}
			}
		}
		return nullptr;
	};
	int s0, s1;
	auto state = output(*ae.GetChildren()[0], s0), count = output(*ae.GetChildren()[1], s1);
	return state && count && s0 != s1 && IsCountStar(*state, AggregateStateExportMode::STATE_EXPORT) &&
	       IsCountStar(*count, AggregateStateExportMode::NONE);
}

static unique_ptr<LogicalOperator> TakeScan(unique_ptr<LogicalOperator> &child) {
	unique_ptr<LogicalOperator> *cur = &child;
	while ((*cur)->type == LogicalOperatorType::LOGICAL_PROJECTION ||
	       (*cur)->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		cur = &(*cur)->children[0];
	}
	return std::move(*cur);
}

static unique_ptr<BaseStatistics> ColumnStats(ClientContext &context, LogicalGet &get, idx_t base_column) {
	if (base_column == DConstants::INVALID_INDEX) {
		return nullptr;
	}
	if (get.function.statistics_extended) {
		TableFunctionGetStatisticsInput input(get.bind_data.get(), ColumnIndex(base_column));
		return get.function.statistics_extended(context, input);
	}
	return get.function.statistics ? get.function.statistics(context, get.bind_data.get(), base_column) : nullptr;
}

static bool NoNulls(ClientContext &context, LogicalGet &get, idx_t base_column) {
	auto stats = ColumnStats(context, get, base_column);
	return stats && !stats->CanHaveNull();
}

static bool AllComparisons(const LogicalComparisonJoin &join) {
	for (auto &cond : join.conditions) {
		if (!cond.IsComparison()) {
			return false;
		}
	}
	return true;
}

struct TesseraPushdown {
	uint64_t min_value = 0;
	int64_t topk = -1;
};

static bool TryTessera(ClientContext &context, unique_ptr<LogicalOperator> &plan, const TesseraPushdown &push);

static bool IsFinalize(const Expression &e) {
	return e.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
	       e.Cast<BoundFunctionExpression>().Function().GetName() == "finalize" &&
	       e.Cast<BoundFunctionExpression>().GetChildren().size() == 1;
}

static unique_ptr<LogicalOperator> *AggregateBelow(LogicalOperator &op, ColumnBinding b, bool &is_value) {
	is_value = false;
	unique_ptr<LogicalOperator> *slot = &op.children[0];
	bool track = true;
	while ((*slot)->type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto &proj = (*slot)->Cast<LogicalProjection>();
		if (track) {
			const Expression *e = b.table_index == proj.table_index && b.column_index < proj.expressions.size()
			                          ? proj.expressions[b.column_index].get()
			                          : nullptr;
			if (e && IsFinalize(*e)) {
				e = e->Cast<BoundFunctionExpression>().GetChildren()[0].get();
			}
			if (!e || e->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
				track = false;
			} else {
				b = e->Cast<BoundColumnRefExpression>().Binding();
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
		if (!BoundComparisonExpression::IsComparison(*e)) {
			continue;
		}
		auto &cmp = e->Cast<BoundFunctionExpression>();
		const Expression *col = &BoundComparisonExpression::Left(cmp), *cst = &BoundComparisonExpression::Right(cmp);
		ExpressionType t = cmp.GetExpressionType();
		if (col->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			std::swap(col, cst);
			t = FlipComparisonExpression(t);
		}
		if (col->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF ||
		    cst->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
			continue;
		}
		bool is_value;
		auto s = AggregateBelow(filter, col->Cast<BoundColumnRefExpression>().Binding(), is_value);
		if (!s || !is_value) {
			continue;
		}
		auto &v = cst->Cast<BoundConstantExpression>().GetValue();
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
	    top.orders[0].expression->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return nullptr;
	}
	bool is_value;
	auto s = AggregateBelow(top, top.orders[0].expression->Cast<BoundColumnRefExpression>().Binding(), is_value);
	if (!s || !is_value || top.limit == 0 || top.limit > (1ull << 40)) {
		return nullptr;
	}
	push.topk = (int64_t)top.limit;
	return s;
}

static constexpr double MIN_EXPANSION = 6.0;
static constexpr double MIN_EXPANSION_CROSS = 6.0;
static double Expansion(ClientContext &context, LogicalGet &ga, idx_t ka, LogicalGet *gb, idx_t kb) {
	auto rows = [&](LogicalGet &g) -> double {
		if (!g.function.cardinality) {
			return -1;
		}
		auto st = g.function.cardinality(context, g.bind_data.get());
		return st && st->has_estimated_cardinality ? (double)st->estimated_cardinality : -1;
	};
	auto distinct = [&](LogicalGet &g, idx_t c) -> double {
		auto st = ColumnStats(context, g, c);
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
	vector<Layer> projs;
	unique_ptr<LogicalOperator> *slot = &plan->children[0];
	while ((*slot)->type == LogicalOperatorType::LOGICAL_PROJECTION) {
		projs.push_back(LayerOf((*slot)->Cast<LogicalProjection>()));
		slot = &(*slot)->children[0];
	}
	if ((*slot)->type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		return false;
	}
	auto &join = (*slot)->Cast<LogicalComparisonJoin>();
	ScanSide scan_l, scan_r;
	if (join.join_type != JoinType::INNER || !AllComparisons(join) || !join.duplicate_eliminated_columns.empty() ||
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
	ColumnBinding g0 = g0e->Cast<BoundColumnRefExpression>().Binding();
	info.x_bind = x.l;
	info.x_type = agg.groups[0]->GetReturnType();
	info.x_str = IsStringKey(g0e->GetReturnType());
	if (info.x_str ? !(info.x_compress[0] || info.x_type == g0e->GetReturnType()) || info.x_off[0] || info.x_off[1] ||
	                     (bool)info.x_compress[0] != (bool)info.x_compress[1]
	               : !IsIntegerKey(info.x_type) || !IsIntegerKey(g0e->GetReturnType()) || info.x_compress[0] ||
	                     info.x_compress[1]) {
		return false;
	}
	if (agg.expressions.empty()) {
		info.distinct_only = true;
		info.has_value = false;
		if (push.min_value > 0 || push.topk > 0) {
			return false;
		}
	} else if (agg.expressions[0]->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		return false;
	}
	const bool eager = scan_l.eager || scan_r.eager;
	if (eager && (info.distinct_only ||
	              !EagerCount(agg.expressions[0]->Cast<BoundAggregateExpression>(), projs, scan_l, scan_r))) {
		return false;
	}
	if (eager) {
		info.has_value = false;
		info.eager = true;
		info.agg_type = LogicalType::BIGINT;
	} else if (!info.distinct_only) {
		auto &ae = agg.expressions[0]->Cast<BoundAggregateExpression>();
		if (ae.IsDistinct() || ae.GetFilter() || ae.GetOrderBys()) {
			return false;
		}
		info.agg_type = ae.GetReturnType();
		vector<Layer> below;
		Expression *prod = ae.GetChildren().size() == 1 ? Defined(*ae.GetChildren()[0], projs, below) : nullptr;
		if (ae.Function().GetName() == "count_star" && ae.GetChildren().empty()) {
			info.has_value = false;
		} else if (ae.Function().GetName() == "sum" && prod &&
		           prod->GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
			auto &mul = prod->Cast<BoundFunctionExpression>();
			if (mul.Function().GetName() != "*" || mul.GetChildren().size() != 2) {
				return false;
			}
			int64_t voff[2] = {0, 0};
			auto v0 =
			    ToScan(ThroughProjections(*mul.GetChildren()[0], below, &voff[0]), scan_l, scan_r, false, &voff[0]);
			auto v1 =
			    ToScan(ThroughProjections(*mul.GetChildren()[1], below, &voff[1]), scan_l, scan_r, false, &voff[1]);
			if (voff[0] != 0 || voff[1] != 0) {
				return false;
			}
			SidePair v;
			if (!v0 || !v1 || !SameColumnBothSides(*v0, *v1, gl, gr, v)) {
				return false;
			}
			info.has_value = true;
			info.v_bind = v.l;
			info.v_type = v0->GetReturnType();
			if (!IsIntegerKey(info.v_type) && info.v_type.id() != LogicalTypeId::DOUBLE &&
			    info.v_type.id() != LogicalTypeId::FLOAT) {
				return false;
			}
			info.v_int = IsIntegerKey(info.v_type);
			info.prod_type = mul.GetReturnType();
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
		if (cond.GetComparisonType() == ExpressionType::COMPARE_EQUAL && !have_eq) {
			SidePair k;
			auto kl = ToScan(&cond.GetLHS(), scan_l, scan_r, true), kr = ToScan(&cond.GetRHS(), scan_l, scan_r, true);
			if (!kl || !kr || !SameColumnBothSides(*kl, *kr, gl, gr, k)) {
				return false;
			}
			info.k_bind = k.l;
			info.k_str = IsStringKey(kl->GetReturnType());
			if (!info.k_str && !IsIntegerKey(kl->GetReturnType())) {
				return false;
			}
			have_eq = true;
			continue;
		}
		bool lt = cond.GetComparisonType() == ExpressionType::COMPARE_LESSTHAN ||
		          cond.GetComparisonType() == ExpressionType::COMPARE_LESSTHANOREQUALTO;
		bool gt = cond.GetComparisonType() == ExpressionType::COMPARE_GREATERTHAN ||
		          cond.GetComparisonType() == ExpressionType::COMPARE_GREATERTHANOREQUALTO;
		if ((lt || gt) && !have_cmp) {
			SidePair c;
			ColumnBinding left_b;
			auto cl = ToScan(&cond.GetLHS(), scan_l, scan_r, true), cr = ToScan(&cond.GetRHS(), scan_l, scan_r, true);
			if (!cl || !cr || !SameColumnBothSides(*cl, *cr, gl, gr, c, &left_b) || !(c.l == x.l)) {
				return false;
			}
			ColumnBinding u = left_b;
			if (gt) {
				u = (left_b == c.l) ? c.r : c.l;
			}
			bool eq = cond.GetComparisonType() == ExpressionType::COMPARE_LESSTHANOREQUALTO ||
			          cond.GetComparisonType() == ExpressionType::COMPARE_GREATERTHANOREQUALTO;
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
	child->Cast<LogicalGet>().dynamic_filters = nullptr;
	if (info.eager) {
		agg_owned->expressions[0] = make_uniq<BoundConstantExpression>(Value(LogicalType::BIGINT));
	}
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
	    !agg.grouping_functions.empty() ||
	    agg.expressions[0]->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		return false;
	}
	vector<Layer> projs;
	unique_ptr<LogicalOperator> *slot = &plan->children[0];
	while ((*slot)->type == LogicalOperatorType::LOGICAL_PROJECTION) {
		projs.push_back(LayerOf((*slot)->Cast<LogicalProjection>()));
		slot = &(*slot)->children[0];
	}
	if ((*slot)->type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		return false;
	}
	auto &join = (*slot)->Cast<LogicalComparisonJoin>();
	ScanSide scan_l, scan_r;
	if (join.join_type != JoinType::INNER || !AllComparisons(join) || !join.duplicate_eliminated_columns.empty() ||
	    join.children.size() != 2 || !ScanBelow(*join.children[0], scan_l) || !ScanBelow(*join.children[1], scan_r) ||
	    join.conditions.size() != 1 || join.conditions[0].GetComparisonType() != ExpressionType::COMPARE_EQUAL) {
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
		const bool str = IsStringKey(ge[g]->GetReturnType());
		if (str ? info.x_off[g] != 0 ||
		              (!info.x_compress[g] && agg.groups[g]->GetReturnType() != ge[g]->GetReturnType())
		        : !IsIntegerKey(ge[g]->GetReturnType()) || !IsIntegerKey(agg.groups[g]->GetReturnType()) ||
		              info.x_compress[g]) {
			return false;
		}
	}
	if (gs[0] == gs[1] || (gl.GetTable().get() == gr.GetTable().get() && gc[0] == gc[1])) {
		return false;
	}
	info.x_type = agg.groups[0]->GetReturnType();
	info.y_type = agg.groups[1]->GetReturnType();
	info.x_str = IsStringKey(ge[0]->GetReturnType());
	info.y_str = IsStringKey(ge[1]->GetReturnType());
	const int sx = gs[0];
	LogicalGet &gx = sx == 0 ? gl : gr, &gy = sx == 0 ? gr : gl;
	auto &cond = join.conditions[0];
	auto kle = ToScan(&cond.GetLHS(), scan_l, scan_r, true), kre = ToScan(&cond.GetRHS(), scan_l, scan_r, true);
	int sl, sr;
	idx_t cl, cr;
	ColumnBinding bl, br;
	if (!kle || !kre || !SideOf(*kle, gl, gr, sl, cl, bl) || !SideOf(*kre, gl, gr, sr, cr, br) || sl == sr) {
		return false;
	}
	const ColumnBinding kx = sl == sx ? bl : br, ky = sl == sx ? br : bl;
	const LogicalType kxt = (sl == sx ? kle : kre)->GetReturnType();
	const LogicalType kyt = (sl == sx ? kre : kle)->GetReturnType();
	info.k_str = IsStringKey(kxt);
	if (!(IsIntegerKey(kxt) && IsIntegerKey(kyt)) && !(info.k_str && IsStringKey(kyt))) {
		return false;
	}
	auto &ae = agg.expressions[0]->Cast<BoundAggregateExpression>();
	if (ae.IsDistinct() || ae.GetFilter() || ae.GetOrderBys()) {
		return false;
	}
	const bool eager = scan_l.eager || scan_r.eager;
	if (eager && !EagerCount(ae, projs, scan_l, scan_r)) {
		return false;
	}
	info.agg_type = eager ? LogicalType::BIGINT : ae.GetReturnType();
	ColumnBinding vb[2];
	LogicalType vt[2];
	vector<Layer> below;
	Expression *prod = ae.GetChildren().size() == 1 ? Defined(*ae.GetChildren()[0], projs, below) : nullptr;
	if (eager || (ae.Function().GetName() == "count_star" && ae.GetChildren().empty())) {
		info.has_value = false;
		info.eager = eager;
	} else if (ae.Function().GetName() == "sum" && prod &&
	           prod->GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &mul = prod->Cast<BoundFunctionExpression>();
		if (mul.Function().GetName() != "*" || mul.GetChildren().size() != 2) {
			return false;
		}
		bool seen[2] = {false, false};
		for (idx_t f = 0; f < 2; f++) {
			int64_t voff = 0;
			auto ve = ToScan(ThroughProjections(*mul.GetChildren()[f], below, &voff), scan_l, scan_r, false, &voff);
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
			vt[side] = ve->GetReturnType();
			if (!IsIntegerKey(vt[side]) && vt[side].id() != LogicalTypeId::DOUBLE && vt[side].id() != LogicalTypeId::FLOAT) {
				return false;
			}
		}
		info.has_value = true;
		info.v_int = IsIntegerKey(vt[0]) && IsIntegerKey(vt[1]);
		info.prod_type = mul.GetReturnType();
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
	const LogicalType xt = ge[0]->GetReturnType(), yt = ge[1]->GetReturnType();
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
			const LogicalType vt_u = info.v_int ? LogicalType::BIGINT : LogicalType::DOUBLE;
			for (idx_t s = 0; s < 2; s++) {
				e.push_back(s == side ? BoundCastExpression::AddCastToType(context, colref(vt[s], vb[s]), vt_u)
				                      : unique_ptr<Expression>(make_uniq<BoundConstantExpression>(Value(vt_u))));
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
	if (info.eager) {
		agg_owned->expressions[0] = make_uniq<BoundConstantExpression>(Value(LogicalType::BIGINT));
	}
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
		if (t->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return false;
		}
		auto &b = t->Cast<BoundColumnRefExpression>().Binding();
		if (b.table_index != proj.table_index || b.column_index > 1 || seen[b.column_index]) {
			return false;
		}
		seen[b.column_index] = true;
	}
	vector<unique_ptr<Expression>> none;
	auto agg = make_uniq<LogicalAggregate>(proj.table_index, TableIndex(), std::move(none));
	for (idx_t c = 0; c < 2; c++) {
		agg->groups.push_back(make_uniq<BoundColumnRefExpression>(proj.expressions[c]->GetReturnType(),
		                                                          ColumnBinding(proj.table_index, ProjectionIndex(c))));
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

static unique_ptr<LogicalOperator> TesseraSimBind(ClientContext &context, TableFunctionBindInput &input,
                                                  TableIndex bind_index, vector<Identifier> &names) {
	if (!tessera::cpu_supported()) {
		throw NotImplementedException("tessera_sim: this CPU lacks x86-64-v3 (AVX2, BMI2, FMA), which Tessera needs");
	}
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
	auto quote = [](const string &id) {
		return SQLIdentifier::ToString(id);
	};
	const auto qn = QualifiedName::Parse(tbl);
	string from;
	for (auto &part : qn.Path()) {
		if (!part.empty()) {
			from += (from.empty() ? "" : ".") + quote(part.GetIdentifierName());
		}
	}
	string sql = "SELECT " + quote(kc) + ", " + quote(xc) + (vc.empty() ? "" : ", " + quote(vc)) + " FROM " + from;
	Parser parser(context);
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
	const TableIndex gi = agg->group_index, ai = agg->aggregate_index;
	auto op = make_uniq<LogicalTessera>(std::move(agg), std::move(child), std::move(info));
	for (idx_t c = 0; c < cb.size(); c++) {
		op->expressions.push_back(make_uniq<BoundColumnRefExpression>(bound.types[c], cb[c]));
	}
	vector<unique_ptr<Expression>> out;
	out.push_back(make_uniq<BoundColumnRefExpression>(xt, ColumnBinding(gi, ProjectionIndex(0))));
	out.push_back(make_uniq<BoundColumnRefExpression>(xt, ColumnBinding(gi, ProjectionIndex(1))));
	out.push_back(make_uniq<BoundColumnRefExpression>(LogicalType::DOUBLE, ColumnBinding(ai, ProjectionIndex(0))));
	auto proj = make_uniq<LogicalProjection>(bind_index, std::move(out));
	proj->children.push_back(std::move(op));
	names = {"x0", "x1", "s"};
	return std::move(proj);
}

static void FinishEagerCounts(ClientContext &context, LogicalOperator &op, column_binding_set_t &carried) {
	for (auto &child : op.children) {
		FinishEagerCounts(context, *child, carried);
	}
	if (op.type == LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR) {
		auto &ext = op.Cast<LogicalExtensionOperator>();
		if (ext.GetExtensionName() == "tessera" && ext.Cast<LogicalTessera>().info.eager) {
			carried.insert(ColumnBinding(ext.Cast<LogicalTessera>().agg_op->aggregate_index, ProjectionIndex(0)));
		}
		return;
	}
	if (op.type != LogicalOperatorType::LOGICAL_PROJECTION || carried.empty()) {
		return;
	}
	auto &proj = op.Cast<LogicalProjection>();
	auto carries = [&](const Expression &e) {
		return e.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
		       carried.count(e.Cast<BoundColumnRefExpression>().Binding());
	};
	std::function<void(unique_ptr<Expression> &)> finish = [&](unique_ptr<Expression> &e) {
		if (IsFinalize(*e) && carries(*e->Cast<BoundFunctionExpression>().GetChildren()[0])) {
			auto type = e->GetReturnType();
			auto count = std::move(e->Cast<BoundFunctionExpression>().GetChildrenMutable()[0]);
			count->SetReturnType(LogicalType::BIGINT);
			e = BoundCastExpression::AddCastToType(context, std::move(count), type);
			return;
		}
		ExpressionIterator::EnumerateChildren(*e, finish);
	};
	for (idx_t i = 0; i < proj.expressions.size(); i++) {
		auto &e = proj.expressions[i];
		if (carries(*e)) {
			e->SetReturnType(LogicalType::BIGINT);
			carried.insert(ColumnBinding(proj.table_index, ProjectionIndex(i)));
		} else {
			finish(e);
		}
	}
}

static void TesseraOptimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	if (!tessera::cpu_supported()) {
		return;
	}
	Value enabled;
	if (input.context.TryGetCurrentSetting("tessera_enabled", enabled) &&
	    !enabled.IsNull() && !enabled.GetValue<bool>()) {
		return;
	}
	Inject(input.context, plan, input.optimizer.binder);
	column_binding_set_t carried;
	FinishEagerCounts(input.context, *plan, carried);
	if (!carried.empty()) {
		plan->ResolveOperatorTypes();
	}
}

static void LoadInternal(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	OptimizerExtension ext;
	ext.optimize_function = TesseraOptimize;
	OptimizerExtension::Register(config, std::move(ext));
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
	sim.GetSignature().AddKeywordOnly("v", LogicalType::VARCHAR, Value(LogicalType::VARCHAR));
	sim.GetSignature().AddKeywordOnly("mode", LogicalType::VARCHAR, Value("lt"));
	loader.RegisterFunction(sim);
}

void TesseraExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string TesseraExtension::Name() {
	return "tessera";
}

std::string TesseraExtension::Version() const {
	return tessera::kVersion;
}

}

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(tessera, loader) {
	duckdb::LoadInternal(loader);
}
}
