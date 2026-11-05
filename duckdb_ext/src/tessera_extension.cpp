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
#include "duckdb/planner/operator/logical_window.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/planner/table_filter_set.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/execution/expression_executor.hpp"
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

static bool IsTemporalKey(const LogicalType &t) {
	switch (t.id()) {
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ:
		return true;
	default:
		return false;
	}
}

static bool IsIntegerKey(const LogicalType &t);
static Expression &WithoutDecimalWidening(Expression &e) {
	if (BoundCastExpression::IsCast(e)) {
		auto &f = e.Cast<BoundFunctionExpression>();
		auto &child = const_cast<Expression &>(BoundCastExpression::Child(f));
		const auto &from = child.GetReturnType(), &to = e.GetReturnType();
		if (from.id() == LogicalTypeId::DECIMAL && to.id() == LogicalTypeId::DECIMAL &&
		    DecimalType::GetScale(from) == DecimalType::GetScale(to) &&
		    DecimalType::GetWidth(from) <= DecimalType::GetWidth(to) && !BoundCastExpression::IsTryCast(f)) {
			return child;
		}
	}
	return e;
}

static Expression *WithoutIntegerWidening(Expression *e) {
	if (!BoundCastExpression::IsCast(*e)) {
		return e;
	}
	auto &f = e->Cast<BoundFunctionExpression>();
	auto &child = const_cast<Expression &>(BoundCastExpression::Child(f));
	const auto &from = child.GetReturnType(), &to = e->GetReturnType();
	if (!IsIntegerKey(from) || !IsIntegerKey(to) || BoundCastExpression::IsTryCast(f)) {
		return e;
	}
	const auto unsigned_type = [](const LogicalType &t) {
		return t.id() == LogicalTypeId::UTINYINT || t.id() == LogicalTypeId::USMALLINT || t.id() == LogicalTypeId::UINTEGER;
	};
	const idx_t fb = GetTypeIdSize(from.InternalType()), tb = GetTypeIdSize(to.InternalType());
	const bool widens = unsigned_type(from) == unsigned_type(to) ? tb >= fb : unsigned_type(from) && tb > fb;
	return widens ? &child : e;
}

static bool IsDecimalWeight(const LogicalType &t) {
	return t.id() == LogicalTypeId::DECIMAL && DecimalType::GetWidth(t) <= 18;
}
static bool IsExactWeight(const LogicalType &t) {
	return IsIntegerKey(t) || IsDecimalWeight(t);
}
static bool IsOrderedKey(const LogicalType &t) {
	return IsIntegerKey(t) || IsTemporalKey(t);
}

static bool IsTextKey(const LogicalType &t) {
	switch (t.id()) {
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::UUID:
	case LogicalTypeId::BOOLEAN:
		return true;
	default:
		return false;
	}
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
                                      shared_ptr<Expression> *str_compress = nullptr, bool raw = false) {
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
			if (!raw) {
				auto min = f.GetChildren()[1]->Cast<BoundConstantExpression>().GetValue().DefaultTryCastAs(LogicalType::BIGINT);
				if (!min || min->IsNull()) {
					return nullptr;
				}
				*offset += comp ? min->GetValue<int64_t>() : -min->GetValue<int64_t>();
			}
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
	idx_t depth = 0;
	LogicalAggregate *eager = nullptr;
	LogicalFilter *filter = nullptr;
	LogicalGet *get = nullptr;
	LogicalOperator *bottom = nullptr;
};

static bool ScanBelow(LogicalOperator &child, ScanSide &s, bool opaque = false, bool eager = true) {
	LogicalOperator *cur = &child;
	while (cur->children.size() == 1) {
		if (cur->type == LogicalOperatorType::LOGICAL_PROJECTION) {
			s.layers.push_back(LayerOf(cur->Cast<LogicalProjection>()));
		} else if (cur->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY && !s.eager && eager) {
			s.eager = &cur->Cast<LogicalAggregate>();
			s.layers.push_back({s.eager->group_index, &s.eager->groups});
		} else {
			break;
		}
		s.depth++;
		cur = cur->children[0].get();
	}
	LogicalOperator *below = cur;
	if (cur->type == LogicalOperatorType::LOGICAL_FILTER && cur->children.size() == 1 &&
	    cur->children[0]->type == LogicalOperatorType::LOGICAL_GET) {
		s.filter = &cur->Cast<LogicalFilter>();
		cur = cur->children[0].get();
	}
	if (cur->type == LogicalOperatorType::LOGICAL_GET) {
		s.get = &cur->Cast<LogicalGet>();
		return true;
	}
	if (!opaque) {
		return false;
	}
	s.filter = nullptr;
	s.bottom = below;
	return true;
}

static unique_ptr<LogicalOperator> TakeBelow(unique_ptr<LogicalOperator> &child, idx_t depth) {
	unique_ptr<LogicalOperator> *cur = &child;
	for (idx_t i = 0; i < depth; i++) {
		cur = &(*cur)->children[0];
	}
	return std::move(*cur);
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
			return key ? ThroughProjections(*e, s->layers, &offset, &str_compress, true)
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

struct EagerAgg {
	bool ok = false;
	bool sum = false;
	bool avg = false;
	LogicalType avg_input;
	int side = 0;
	Expression *arg = nullptr;
};

static Expression *BelowEager(Expression &e, const ScanSide &s) {
	idx_t i = 0;
	while (i < s.layers.size() && s.layers[i].exprs != &s.eager->groups) {
		i++;
	}
	vector<Layer> rest(s.layers.begin() + MinValue<idx_t>(i + 1, s.layers.size()), s.layers.end());
	int64_t off = 0;
	auto out = ThroughProjections(e, rest, &off);
	return off == 0 ? out : nullptr;
}

static EagerAgg EagerCount(const BoundAggregateExpression &ae, const vector<Layer> &above, const ScanSide &l,
                           const ScanSide &r) {
	EagerAgg res;
	if (!l.eager || !r.eager || ae.Function().GetName() != "combine_aggr" || ae.GetChildren().size() != 2 ||
	    ae.IsDistinct() || ae.GetFilter() || ae.GetOrderBys()) {
		return res;
	}
	for (auto *s : {&l, &r}) {
		if (s->eager->grouping_sets.size() > 1 || !s->eager->grouping_functions.empty()) {
			return res;
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
	if (!state || !count || s0 == s1 || !IsCountStar(*count, AggregateStateExportMode::NONE) ||
	    state->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
		return res;
	}
	res.side = s0;
	if (IsCountStar(*state, AggregateStateExportMode::STATE_EXPORT)) {
		res.ok = true;
		return res;
	}
	auto &a = state->Cast<BoundAggregateExpression>();
	const auto &name = a.Function().GetName();
	if ((name != "count" && name != "sum" && name != "avg") || a.GetChildren().size() != 1 || a.IsDistinct() ||
	    a.GetFilter() || a.GetOrderBys() || a.StateExportMode() != AggregateStateExportMode::STATE_EXPORT) {
		return res;
	}
	res.arg = BelowEager(*a.GetChildren()[0], s0 == 0 ? l : r);
	res.sum = name == "sum" || name == "avg";
	res.avg = name == "avg";
	res.avg_input = a.GetChildren()[0]->GetReturnType();
	res.ok = res.arg && res.arg->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF;
	return res;
}

static LogicalAggregate *DistinctBelow(unique_ptr<LogicalOperator> *&slot, vector<Layer> &projs, idx_t &below) {
	if ((*slot)->type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		return nullptr;
	}
	auto &d = (*slot)->Cast<LogicalAggregate>();
	if (!d.expressions.empty() || d.groups.size() != 3 || d.grouping_sets.size() > 1 || !d.grouping_functions.empty()) {
		return nullptr;
	}
	projs.push_back({d.group_index, &d.groups});
	below = projs.size();
	slot = &(*slot)->children[0];
	while ((*slot)->type == LogicalOperatorType::LOGICAL_PROJECTION) {
		projs.push_back(LayerOf((*slot)->Cast<LogicalProjection>()));
		slot = &(*slot)->children[0];
	}
	return &d;
}

static bool DistinctGroups(LogicalAggregate &d, const vector<Layer> &projs, idx_t below, ScanSide &l, ScanSide &r,
                           const vector<ColumnBinding> &groups, const vector<ColumnBinding> &keys) {
	vector<Layer> rest(projs.begin() + below, projs.end());
	idx_t nk = 0, ng = 0;
	for (auto &g : d.groups) {
		int64_t off = 0;
		shared_ptr<Expression> str_compress;
		auto e = ToScan(ThroughProjections(*g, rest, &off, &str_compress), l, r, true);
		int side;
		idx_t base;
		ColumnBinding b;
		if (!e || !SideOf(*e, *l.get, *r.get, side, base, b)) {
			return false;
		}
		if (std::find(keys.begin(), keys.end(), b) != keys.end()) {
			nk++;
		} else if (std::find(groups.begin(), groups.end(), b) != groups.end()) {
			ng++;
		} else {
			return false;
		}
	}
	return nk == 1 && ng == 2;
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

static bool SameTableFilters(LogicalGet &a, LogicalGet &b) {
	auto &fa = a.table_filters, &fb = b.table_filters;
	if (fa.HasMultiColumnFilters() || fb.HasMultiColumnFilters()) {
		auto &ia = a.GetColumnIds(), &ib = b.GetColumnIds();
		bool same = ia.size() == ib.size();
		for (idx_t i = 0; same && i < ia.size(); i++) {
			same = ia[i].GetPrimaryIndex() == ib[i].GetPrimaryIndex() && !ia[i].HasChildren() && !ib[i].HasChildren();
		}
		return same && TableFilterSet::Equals(&fa, &fb);
	}
	if (fa.FilterCount() != fb.FilterCount()) {
		return false;
	}
	auto base = [](LogicalGet &g, idx_t p) {
		auto &ids = g.GetColumnIds();
		return p < ids.size() && !ids[p].HasChildren() ? ids[p].GetPrimaryIndex() : DConstants::INVALID_INDEX;
	};
	for (auto &ea : fa) {
		const idx_t col = base(a, ea.GetIndex());
		bool same = false;
		for (auto &eb : fb) {
			if (base(b, eb.GetIndex()) == col) {
				same = col != DConstants::INVALID_INDEX &&
				       ea.Filter().Cast<ExpressionFilter>().Equals(eb.Filter().Cast<ExpressionFilter>());
				break;
			}
		}
		if (!same) {
			return false;
		}
	}
	return true;
}

static bool TryTessera(ClientContext &context, unique_ptr<LogicalOperator> &plan, const TesseraPushdown &push,
                       bool *limited = nullptr);

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
		if (lo >= 1 && lo < 0x1p53) {
			push.min_value = std::max<uint64_t>(push.min_value, (uint64_t)lo);
			slot = s;
		}
	}
	return slot;
}

static unique_ptr<LogicalOperator> *TopNBelow(LogicalTopN &top, TesseraPushdown &push) {
	if (top.orders.empty() || top.orders[0].type != OrderType::DESCENDING ||
	    top.orders[0].expression->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return nullptr;
	}
	bool is_value;
	auto s = AggregateBelow(top, top.orders[0].expression->Cast<BoundColumnRefExpression>().Binding(), is_value);
	const idx_t imax = (idx_t)NumericLimits<int64_t>::Maximum();
	if (!s || !is_value || top.limit == 0 || top.offset > imax || top.limit > imax - top.offset) {
		return nullptr;
	}
	push.topk = (int64_t)(top.limit + top.offset);
	return s;
}

static constexpr double MIN_EXPANSION = 6.0;
static constexpr double MIN_EXPANSION_CROSS = 6.0;
static double TableRows(ClientContext &context, LogicalGet &g) {
	if (!g.function.cardinality) {
		return -1;
	}
	auto st = g.function.cardinality(context, g.bind_data.get());
	return st && st->has_estimated_cardinality ? (double)st->estimated_cardinality : -1;
}
static double ColumnDistinct(ClientContext &context, LogicalGet &g, idx_t c) {
	auto st = ColumnStats(context, g, c);
	return st && st->GetDistinctCount() > 0 ? (double)st->GetDistinctCount() : -1;
}
static double Expansion(ClientContext &context, LogicalGet &ga, idx_t ka, LogicalGet *gb, idx_t kb) {
	auto rows = [&](LogicalGet &g) {
		return TableRows(context, g);
	};
	auto distinct = [&](LogicalGet &g, idx_t c) {
		return ColumnDistinct(context, g, c);
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

static bool TryTessera(ClientContext &context, unique_ptr<LogicalOperator> &plan, const TesseraPushdown &push,
                       bool *limited) {
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
	idx_t dist_below = 0;
	LogicalAggregate *dist = DistinctBelow(slot, projs, dist_below);
	if ((*slot)->type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		return false;
	}
	auto &join = (*slot)->Cast<LogicalComparisonJoin>();
	ScanSide scan_l, scan_r;
	if (join.join_type != JoinType::INNER || !AllComparisons(join) || !join.duplicate_eliminated_columns.empty() ||
	    join.children.size() != 2 || !ScanBelow(*join.children[0], scan_l) || !ScanBelow(*join.children[1], scan_r) ||
	    scan_l.filter || scan_r.filter) {
		return false;
	}
	auto &gl = *scan_l.get;
	auto &gr = *scan_r.get;
	auto tl = gl.GetTable(), tr = gr.GetTable();
	if (!tl || !tr || tl.get() != tr.get() || !SameTableFilters(gl, gr)) {
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
	               : !IsOrderedKey(info.x_type) || !IsOrderedKey(g0e->GetReturnType()) || info.x_compress[0] ||
	                     info.x_compress[1]) {
		return false;
	}
	Expression *distinct_key = nullptr;
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
	if (eager) {
		auto ea = info.distinct_only ? EagerAgg()
		                             : EagerCount(agg.expressions[0]->Cast<BoundAggregateExpression>(), projs, scan_l, scan_r);
		if (!ea.ok || ea.sum) {
			return false;
		}
		int side;
		idx_t base;
		ColumnBinding b;
		if (ea.arg && (!SideOf(*ea.arg, gl, gr, side, base, b) || !NoNulls(context, side == 0 ? gl : gr, base))) {
			return false;
		}
	}
	if (eager) {
		info.has_value = false;
		info.eager = true;
		info.agg_type = LogicalType::BIGINT;
	} else if (!info.distinct_only) {
		auto &ae = agg.expressions[0]->Cast<BoundAggregateExpression>();
		if (ae.GetFilter() || ae.GetOrderBys() ||
		    (ae.IsDistinct() && (ae.Function().GetName() != "count" || ae.GetChildren().size() != 1))) {
			return false;
		}
		info.agg_type = ae.GetReturnType();
		vector<Layer> below;
		Expression *prod = ae.GetChildren().size() == 1 ? Defined(*ae.GetChildren()[0], projs, below) : nullptr;
		if (ae.IsDistinct()) {
			int64_t off = 0;
			shared_ptr<Expression> str_compress;
			distinct_key = ToScan(ThroughProjections(*ae.GetChildren()[0], projs, &off, &str_compress), scan_l, scan_r, true);
			if (!distinct_key) {
				return false;
			}
			info.has_value = false;
			info.distinct_keys = true;
		} else if (ae.Function().GetName() == "count_star" && ae.GetChildren().empty()) {
			info.has_value = false;
		} else if (ae.Function().GetName() == "sum" && prod &&
		           prod->GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
			auto &mul = prod->Cast<BoundFunctionExpression>();
			if (mul.Function().GetName() != "*" || mul.GetChildren().size() != 2) {
				return false;
			}
			int64_t voff[2] = {0, 0};
			auto v0 = ToScan(ThroughProjections(WithoutDecimalWidening(*mul.GetChildren()[0]), below, &voff[0]), scan_l,
			                 scan_r, false, &voff[0]);
			auto v1 = ToScan(ThroughProjections(WithoutDecimalWidening(*mul.GetChildren()[1]), below, &voff[1]), scan_l,
			                 scan_r, false, &voff[1]);
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
			if (!IsExactWeight(info.v_type) && info.v_type.id() != LogicalTypeId::DOUBLE &&
			    info.v_type.id() != LogicalTypeId::FLOAT) {
				return false;
			}
			info.v_int = IsExactWeight(info.v_type);
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
	ColumnBinding k_l, k_r;
	for (auto &cond : join.conditions) {
		if (cond.GetComparisonType() == ExpressionType::COMPARE_EQUAL && !have_eq) {
			SidePair k;
			auto kl = ToScan(&cond.GetLHS(), scan_l, scan_r, true), kr = ToScan(&cond.GetRHS(), scan_l, scan_r, true);
			if (!kl || !kr || !SameColumnBothSides(*kl, *kr, gl, gr, k)) {
				return false;
			}
			info.k_bind = k.l;
			k_l = k.l;
			k_r = k.r;
			info.k_str = IsStringKey(kl->GetReturnType());
			if (!info.k_str && !IsOrderedKey(kl->GetReturnType())) {
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
		if ((cond.GetComparisonType() == ExpressionType::COMPARE_NOTEQUAL ||
		     cond.GetComparisonType() == ExpressionType::COMPARE_DISTINCT_FROM) &&
		    !have_cmp) {
			SidePair c;
			auto cl = ToScan(&cond.GetLHS(), scan_l, scan_r, true), cr = ToScan(&cond.GetRHS(), scan_l, scan_r, true);
			if (!cl || !cr || !SameColumnBothSides(*cl, *cr, gl, gr, c) || !(c.l == x.l)) {
				return false;
			}
			info.mode = TesseraPairMode::NE;
			have_cmp = true;
			continue;
		}
		return false;
	}
	if (!have_eq || info.k_bind == info.x_bind || (info.has_value && (info.v_bind == info.k_bind))) {
		return false;
	}
	if (distinct_key) {
		int side;
		idx_t base;
		ColumnBinding b;
		if (!SideOf(*distinct_key, gl, gr, side, base, b) || base != BaseColumn(gl, info.k_bind)) {
			return false;
		}
	}
	if (dist) {
		if (info.has_value || info.distinct_only || info.eager || info.distinct_keys ||
		    !DistinctGroups(*dist, projs, dist_below, scan_l, scan_r, {x.l, x.r}, {k_l, k_r})) {
			return false;
		}
		info.distinct_keys = true;
	}
	if (!NoNulls(context, gl, BaseColumn(gl, info.x_bind)) ||
	    (info.has_value && !NoNulls(context, gl, BaseColumn(gl, info.v_bind)))) {
		return false;
	}
	if (!JoinExpands(context, Expansion(context, gl, BaseColumn(gl, info.k_bind), nullptr, 0), false)) {
		if (limited) {
			*limited = true;
		}
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

struct Input {
	LogicalGet *get = nullptr;
	LogicalOperator *op = nullptr;
	vector<ColumnBinding> binds;
};

static idx_t InputColumn(const Input &in, const ColumnBinding &b) {
	if (in.get) {
		return BaseColumn(*in.get, b);
	}
	for (idx_t i = 0; i < in.binds.size(); i++) {
		if (in.binds[i] == b) {
			return i;
		}
	}
	return DConstants::INVALID_INDEX;
}

static bool SideOf(Expression &e, const Input &l, const Input &r, int &side, idx_t &base, ColumnBinding &b) {
	if (e.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return false;
	}
	b = e.Cast<BoundColumnRefExpression>().Binding();
	for (side = 0; side < 2; side++) {
		base = InputColumn(side == 0 ? l : r, b);
		if (base != DConstants::INVALID_INDEX) {
			return true;
		}
	}
	return false;
}

static bool NoNulls(ClientContext &context, const Input &in, idx_t base) {
	return in.get && NoNulls(context, *in.get, base);
}

static LogicalGet *ScanColumnOf(LogicalOperator *op, ColumnBinding b, idx_t &base) {
	while (op) {
		switch (op->type) {
		case LogicalOperatorType::LOGICAL_GET: {
			auto &g = op->Cast<LogicalGet>();
			base = BaseColumn(g, b);
			return base == DConstants::INVALID_INDEX ? nullptr : &g;
		}
		case LogicalOperatorType::LOGICAL_PROJECTION:
		case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY: {
			const bool proj = op->type == LogicalOperatorType::LOGICAL_PROJECTION;
			auto &exprs = proj ? op->expressions : op->Cast<LogicalAggregate>().groups;
			const TableIndex index = proj ? op->Cast<LogicalProjection>().table_index : op->Cast<LogicalAggregate>().group_index;
			if (b.table_index != index || b.column_index >= exprs.size() ||
			    exprs[b.column_index]->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
				return nullptr;
			}
			b = exprs[b.column_index]->Cast<BoundColumnRefExpression>().Binding();
			break;
		}
		case LogicalOperatorType::LOGICAL_WINDOW:
			if (b.table_index == op->Cast<LogicalWindow>().window_index) {
				return nullptr;
			}
			break;
		case LogicalOperatorType::LOGICAL_FILTER:
		case LogicalOperatorType::LOGICAL_DISTINCT:
		case LogicalOperatorType::LOGICAL_ORDER_BY:
		case LogicalOperatorType::LOGICAL_LIMIT:
		case LogicalOperatorType::LOGICAL_TOP_N:
			break;
		default:
			return nullptr;
		}
		op = op->children.size() == 1 ? op->children[0].get() : nullptr;
	}
	return nullptr;
}

static bool InputStats(ClientContext &context, const Input &in, const ColumnBinding &k, double &n, double &v) {
	if (in.get) {
		n = TableRows(context, *in.get);
		v = ColumnDistinct(context, *in.get, BaseColumn(*in.get, k));
	} else {
		idx_t base;
		LogicalGet *g = in.op->has_estimated_cardinality ? ScanColumnOf(in.op, k, base) : nullptr;
		if (!g) {
			return false;
		}
		n = (double)in.op->estimated_cardinality;
		v = ColumnDistinct(context, *g, base);
	}
	return n >= 0 && v > 0;
}

static bool MayThrow(ClientContext &context, const Expression &e, const Input &in) {
	bool kids = false, kids_duck = false;
	ExpressionIterator::EnumerateChildren(e, [&](const Expression &c) {
		kids = kids || MayThrow(context, c, in);
		kids_duck = kids_duck || c.CanThrow();
	});
	if (kids) {
		return true;
	}
	if (e.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &f = e.Cast<BoundFunctionExpression>();
	if (f.Function().GetErrorMode() != FunctionErrors::CAN_THROW_RUNTIME_ERROR) {
		return f.CanThrow() && !kids_duck;
	}
	if (BoundCastExpression::IsCast(e)) {
		return true;
	}
	auto &ch = f.GetChildren();
	const auto &n = f.Function().GetName();
	auto constant = [&](idx_t i) -> const Value * {
		return i < ch.size() && ch[i]->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT &&
		               !ch[i]->Cast<BoundConstantExpression>().GetValue().IsNull()
		           ? &ch[i]->Cast<BoundConstantExpression>().GetValue()
		           : nullptr;
	};
	if ((n == "%" || n == "mod" || n == "//" || n == "fdiv") && ch.size() == 2 && constant(1) &&
	    IsIntegerKey(constant(1)->type())) {
		const int64_t d = constant(1)->GetValue<int64_t>();
		return d == 0 || d == -1;
	}
	const bool part = n == "date_part" || n == "datepart", trunc = n == "date_trunc" || n == "datetrunc";
	if ((part || trunc) && ch.size() == 2 && constant(0) && constant(0)->type().id() == LogicalTypeId::VARCHAR) {
		static const char *parts[] = {"millennium", "century", "decade", "year", "quarter", "month", "week",
		                              "day", "hour", "minute", "second", "millisecond", "microsecond"};
		const string p = StringUtil::Lower(constant(0)->GetValue<string>());
		bool known = false;
		for (auto *q : parts) {
			known = known || p == q;
		}
		const auto t = ch[1]->GetReturnType().id();
		if (!known || (t != LogicalTypeId::DATE && t != LogicalTypeId::TIMESTAMP)) {
			return true;
		}
		if (part) {
			return false;
		}
		if (!in.get || ch[1]->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return true;
		}
		auto st = ColumnStats(context, *in.get, BaseColumn(*in.get, ch[1]->Cast<BoundColumnRefExpression>().Binding()));
		if (!st || !NumericStats::HasMinMax(*st)) {
			return true;
		}
		for (auto &bound : {NumericStats::Min(*st), NumericStats::Max(*st)}) {
			auto probe = f.Copy();
			probe->Cast<BoundFunctionExpression>().GetChildrenMutable()[1] = make_uniq<BoundConstantExpression>(bound);
			try {
				ExpressionExecutor::EvaluateScalar(context, *probe);
			} catch (std::exception &) {
				return true;
			}
		}
		return false;
	}
	return true;
}

static shared_ptr<Expression> ComputedGroup(ClientContext &context, Expression &e, const vector<Layer> &projs,
                                            ScanSide &l, ScanSide &r, const Input &gl, const Input &gr, int &side) {
	vector<Layer> below;
	Expression *def = Defined(e, projs, below);
	if (def->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF || def->IsVolatile() || def->HasSubquery() ||
	    def->HasParameter() || def->IsAggregate() || def->IsWindow()) {
		return nullptr;
	}
	auto copy = def->Copy();
	bool ok = true;
	side = -1;
	std::function<void(unique_ptr<Expression> &)> map = [&](unique_ptr<Expression> &x) {
		if (!ok) {
			return;
		}
		if (x->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			int64_t off = 0;
			shared_ptr<Expression> str_compress;
			auto sc = ToScan(ThroughProjections(*x, below, &off, &str_compress), l, r, false, &off);
			int s;
			idx_t base;
			ColumnBinding b;
			if (!sc || off != 0 || str_compress || !SideOf(*sc, gl, gr, s, base, b) || (side >= 0 && s != side)) {
				ok = false;
				return;
			}
			side = s;
			x = make_uniq<BoundColumnRefExpression>(sc->GetReturnType(), b);
			return;
		}
		if (x->GetExpressionClass() == ExpressionClass::BOUND_REF) {
			ok = false;
			return;
		}
		ExpressionIterator::EnumerateChildren(*x, map);
	};
	map(copy);
	if (!ok || side < 0 || MayThrow(context, *copy, side == 0 ? gl : gr)) {
		return nullptr;
	}
	return shared_ptr<Expression>(copy.release());
}

static shared_ptr<Expression> CastWeight(Expression &e, const vector<Layer> &layers, ScanSide &l, ScanSide &r,
                                         const Input &gl, const Input &gr, int &side, ColumnBinding &b) {
	vector<Layer> below;
	Expression *def = Defined(e, layers, below);
	if (!BoundCastExpression::IsCast(*def) || def->CanThrow() || def->GetReturnType().id() != LogicalTypeId::DOUBLE ||
	    BoundCastExpression::IsTryCast(def->Cast<BoundFunctionExpression>())) {
		return nullptr;
	}
	auto &child = const_cast<Expression &>(BoundCastExpression::Child(def->Cast<BoundFunctionExpression>()));
	int64_t off = 0;
	shared_ptr<Expression> str_compress;
	auto col = ToScan(ThroughProjections(child, below, &off, &str_compress), l, r, false, &off);
	idx_t base;
	if (!col || off != 0 || str_compress || !SideOf(*col, gl, gr, side, base, b) || !IsExactWeight(col->GetReturnType())) {
		return nullptr;
	}
	auto copy = def->Copy();
	BoundCastExpression::ChildMutable(copy->Cast<BoundFunctionExpression>()) =
	    make_uniq<BoundColumnRefExpression>(col->GetReturnType(), b);
	return shared_ptr<Expression>(copy.release());
}

static bool IsDistinctMarker(const BoundAggregateExpression &a);

static bool TryTesseraCross(ClientContext &context, unique_ptr<LogicalOperator> &plan, const TesseraPushdown &push,
                            Binder &binder, bool opaque = false) {
	auto &agg = plan->Cast<LogicalAggregate>();
	if (agg.groups.size() < 2 || agg.expressions.empty() || agg.expressions.size() > 3 || agg.grouping_sets.size() > 1 ||
	    !agg.grouping_functions.empty()) {
		return false;
	}
	for (auto &e : agg.expressions) {
		if (e->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
			return false;
		}
	}
	auto agg_is = [&](idx_t i, const char *name) {
		return agg.expressions[i]->Cast<BoundAggregateExpression>().Function().GetName() == name;
	};
	idx_t nsum = 0, navg = 0, main_agg = DConstants::INVALID_INDEX;
	for (idx_t i = 0; i < agg.expressions.size(); i++) {
		if (!agg_is(i, "sum") && !agg_is(i, "avg")) {
			continue;
		}
		(agg_is(i, "sum") ? nsum : navg)++;
		if (main_agg == DConstants::INVALID_INDEX) {
			main_agg = i;
			continue;
		}
		auto &m = agg.expressions[main_agg]->Cast<BoundAggregateExpression>();
		auto &e = agg.expressions[i]->Cast<BoundAggregateExpression>();
		if (e.GetChildren().size() != 1 || m.GetChildren().size() != 1 || e.IsDistinct() || e.GetFilter() ||
		    e.GetOrderBys() || !e.GetChildren()[0]->Equals(*m.GetChildren()[0])) {
			return false;
		}
	}
	if (nsum > 1 || navg > 1 || (agg.expressions.size() == 3 && nsum + navg != 2)) {
		return false;
	}
	const bool two = agg.expressions.size() >= 2 || navg > 0;
	if (main_agg == DConstants::INVALID_INDEX) {
		main_agg = two ? 1 : 0;
	}
	vector<Layer> projs;
	unique_ptr<LogicalOperator> *slot = &plan->children[0];
	while ((*slot)->type == LogicalOperatorType::LOGICAL_PROJECTION) {
		projs.push_back(LayerOf((*slot)->Cast<LogicalProjection>()));
		slot = &(*slot)->children[0];
	}
	idx_t dist_below = 0;
	LogicalAggregate *dist = DistinctBelow(slot, projs, dist_below);
	if ((*slot)->type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		return false;
	}
	auto &join = (*slot)->Cast<LogicalComparisonJoin>();
	ScanSide scan_l, scan_r;
	if (join.join_type != JoinType::INNER || !AllComparisons(join) || !join.duplicate_eliminated_columns.empty() ||
	    join.children.size() != 2 || join.conditions.empty()) {
		return false;
	}
	Input gl, gr;
	for (idx_t s = 0; s < 2; s++) {
		if (!ScanBelow(*join.children[s], s == 0 ? scan_l : scan_r, opaque)) {
			return false;
		}
	}
	if ((scan_l.eager != nullptr) != (scan_r.eager != nullptr)) {
		if (!opaque) {
			return false;
		}
		for (idx_t s = 0; s < 2; s++) {
			ScanSide &ss = s == 0 ? scan_l : scan_r;
			if (ss.eager) {
				ss = ScanSide();
				ScanBelow(*join.children[s], ss, true, false);
			}
		}
	}
	bool any_opaque = false;
	for (idx_t s = 0; s < 2; s++) {
		ScanSide &ss = s == 0 ? scan_l : scan_r;
		Input &in = s == 0 ? gl : gr;
		if (ss.get) {
			in.get = ss.get;
		} else {
			in.op = ss.bottom;
			in.binds = ss.bottom->GetColumnBindings();
			any_opaque = true;
		}
	}
	if (opaque && !any_opaque) {
		return false;
	}
	TesseraPlanInfo info;
	info.cross = true;
	info.two = two;
	info.count_first = main_agg == 1;
	struct GroupRef {
		int side;
		idx_t base;
		ColumnBinding b;
		LogicalType type;
		shared_ptr<Expression> expr;
		bool text = false;
	};
	vector<GroupRef> groups;
	for (idx_t g = 0; g < agg.groups.size(); g++) {
		TesseraPlanInfo::TupleCol col;
		auto ge = ToScan(ThroughProjections(*agg.groups[g], projs, &col.off, &col.compress), scan_l, scan_r, false,
		                 &col.off);
		GroupRef ref;
		if (!ge || !SideOf(*ge, gl, gr, ref.side, ref.base, ref.b)) {
			col = TesseraPlanInfo::TupleCol();
			ref.expr = ComputedGroup(context, *agg.groups[g], projs, scan_l, scan_r, gl, gr, ref.side);
			if (!ref.expr) {
				return false;
			}
			ref.base = DConstants::INVALID_INDEX;
			col.nullable = true;
			ge = ref.expr.get();
		}
		ref.type = ge->GetReturnType();
		col.type = ref.type;
		col.str = IsStringKey(ref.type);
		if (IsTextKey(ref.type) && col.off == 0 && !col.compress && agg.groups[g]->GetReturnType() == ref.type) {
			col.str = true;
			col.type = LogicalType::VARCHAR;
			col.compress = shared_ptr<Expression>(
			    BoundCastExpression::AddCastToType(context, make_uniq<BoundReferenceExpression>(LogicalType::VARCHAR, 0),
			                                       ref.type)
			        .release());
			ref.text = true;
		} else if (col.str ? col.off != 0 || (!col.compress && agg.groups[g]->GetReturnType() != ref.type)
		                   : !IsOrderedKey(ref.type) || !IsOrderedKey(agg.groups[g]->GetReturnType()) || col.compress) {
			return false;
		}
		const int sx = groups.empty() ? ref.side : groups[0].side;
		auto &cols = ref.side == sx ? info.xcols : info.ycols;
		info.out_groups.emplace_back(ref.side == sx ? 0 : 1, cols.size());
		cols.push_back(std::move(col));
		groups.push_back(std::move(ref));
	}
	const int sx = groups[0].side;
	if (info.xcols.empty() || info.ycols.empty()) {
		return false;
	}
	Input &gx = sx == 0 ? gl : gr, &gy = sx == 0 ? gr : gl;
	auto group_of = [&](int side, const ColumnBinding &b, idx_t &which, idx_t &idx) {
		for (idx_t g = 0; g < groups.size(); g++) {
			if (groups[g].side == side && !groups[g].expr && groups[g].b == b) {
				which = info.out_groups[g].first;
				idx = info.out_groups[g].second;
				return true;
			}
		}
		return false;
	};
	vector<ColumnBinding> kx, ky;
	vector<LogicalType> kxt, kyt;
	vector<shared_ptr<Expression>> kxe, kye;
	for (auto &cond : join.conditions) {
		const bool eq = cond.GetComparisonType() == ExpressionType::COMPARE_EQUAL;
		const auto side_expr = [&](Expression &e) {
			auto *c = ToScan(eq ? &e : WithoutIntegerWidening(&e), scan_l, scan_r, true);
			return eq || !c ? c : WithoutIntegerWidening(c);
		};
		auto le = side_expr(cond.GetLHS()), re = side_expr(cond.GetRHS());
		int sl, sr;
		idx_t cl, cr;
		ColumnBinding bl, br;
		shared_ptr<Expression> el, er;
		const vector<Layer> none;
		if ((!le || !SideOf(*le, gl, gr, sl, cl, bl)) &&
		    !(eq && (el = ComputedGroup(context, cond.GetLHS(), none, scan_l, scan_r, gl, gr, sl)))) {
			return false;
		}
		if ((!re || !SideOf(*re, gl, gr, sr, cr, br)) &&
		    !(eq && (er = ComputedGroup(context, cond.GetRHS(), none, scan_l, scan_r, gl, gr, sr)))) {
			return false;
		}
		if (sl == sr) {
			return false;
		}
		const bool left_x = sl == sx;
		const ColumnBinding bx = left_x ? bl : br, by = left_x ? br : bl;
		const LogicalType tx = (left_x ? (el ? el.get() : le) : (er ? er.get() : re))->GetReturnType();
		const LogicalType ty = (left_x ? (er ? er.get() : re) : (el ? el.get() : le))->GetReturnType();
		const bool same_class = (IsIntegerKey(tx) && IsIntegerKey(ty)) || (IsStringKey(tx) && IsStringKey(ty)) ||
		                        (IsTemporalKey(tx) && tx == ty) || (IsTextKey(tx) && tx == ty);
		const auto ct = cond.GetComparisonType();
		if (ct == ExpressionType::COMPARE_EQUAL) {
			if (!same_class) {
				return false;
			}
			kx.push_back(bx);
			ky.push_back(by);
			kxt.push_back(tx);
			kyt.push_back(ty);
			kxe.push_back(left_x ? el : er);
			kye.push_back(left_x ? er : el);
			info.k_strs.push_back(IsStringKey(tx) || IsTextKey(tx));
			continue;
		}
		ExpressionType op = left_x ? ct : FlipComparisonExpression(ct);
		idx_t wx, ix, wy, iy;
		if (!same_class || !group_of(sx, bx, wx, ix) || !group_of(1 - sx, by, wy, iy) || wx != 0 || wy != 1 ||
		    (op != ExpressionType::COMPARE_LESSTHAN && op != ExpressionType::COMPARE_LESSTHANOREQUALTO &&
		     op != ExpressionType::COMPARE_GREATERTHAN && op != ExpressionType::COMPARE_GREATERTHANOREQUALTO &&
		     op != ExpressionType::COMPARE_NOTEQUAL && op != ExpressionType::COMPARE_DISTINCT_FROM)) {
			return false;
		}
		if (IsTextKey(tx) && op != ExpressionType::COMPARE_NOTEQUAL && op != ExpressionType::COMPARE_DISTINCT_FROM) {
			return false;
		}
		info.filters.push_back({ix, iy, op});
	}
	if (kx.empty()) {
		return false;
	}
	info.nk = kx.size();
	info.k_str = info.k_strs[0];
	EagerAgg ea_count;
	if (two && agg.expressions.size() >= 2 && (scan_l.eager || scan_r.eager)) {
		const EagerAgg e0 = EagerCount(agg.expressions[0]->Cast<BoundAggregateExpression>(), projs, scan_l, scan_r);
		const EagerAgg e1 = EagerCount(agg.expressions[1]->Cast<BoundAggregateExpression>(), projs, scan_l, scan_r);
		if (!e0.ok || !e1.ok || e0.sum == e1.sum) {
			return false;
		}
		main_agg = e0.sum ? 0 : 1;
		ea_count = e0.sum ? e1 : e0;
		info.count_first = main_agg == 1;
	}
	auto &ae = agg.expressions[main_agg]->Cast<BoundAggregateExpression>();
	const bool ae_distinct = ae.IsDistinct() || IsDistinctMarker(ae);
	if (ae.GetFilter() || ae.GetOrderBys() ||
	    (ae_distinct && ((ae.IsDistinct() && ae.Function().GetName() != "count") || ae.GetChildren().size() != 1 ||
	                     info.nk != 1))) {
		return false;
	}
	if (ae_distinct) {
		int64_t off = 0;
		shared_ptr<Expression> str_compress;
		auto dk = ToScan(ThroughProjections(*ae.GetChildren()[0], projs, &off, &str_compress), scan_l, scan_r, true);
		int s;
		idx_t c;
		ColumnBinding b;
		if (!dk || !SideOf(*dk, gl, gr, s, c, b) || kxe[0] || kye[0] || !(b == kx[0] || b == ky[0])) {
			return false;
		}
		info.distinct_keys = true;
	}
	const bool eager = scan_l.eager || scan_r.eager;
	const EagerAgg ea = eager ? EagerCount(ae, projs, scan_l, scan_r) : EagerAgg();
	if (eager && !ea.ok) {
		return false;
	}
	info.agg_type = eager ? LogicalType::BIGINT : ae.GetReturnType();
	ColumnBinding vb[2];
	LogicalType vt[2];
	bool has_w[2] = {false, false};
	shared_ptr<Expression> vcast[2];
	vector<Layer> below;
	Expression *arg = ae.GetChildren().size() == 1 ? Defined(*ae.GetChildren()[0], projs, below) : nullptr;
	auto weight = [&](Expression &e, vector<Layer> &layers) -> bool {
		int64_t voff = 0;
		auto ve = ToScan(ThroughProjections(WithoutDecimalWidening(e), layers, &voff), scan_l, scan_r, false, &voff);
		int s;
		idx_t c;
		ColumnBinding b;
		shared_ptr<Expression> cast;
		if (voff != 0 || !ve || !SideOf(*ve, gl, gr, s, c, b)) {
			cast = CastWeight(e, layers, scan_l, scan_r, gl, gr, s, b);
			if (!cast) {
				return false;
			}
		}
		const idx_t side = s == sx ? 0 : 1;
		if (has_w[side]) {
			return false;
		}
		has_w[side] = true;
		vb[side] = b;
		vcast[side] = cast;
		vt[side] = cast ? cast->GetReturnType() : ve->GetReturnType();
		return IsExactWeight(vt[side]) || vt[side].id() == LogicalTypeId::DOUBLE || vt[side].id() == LogicalTypeId::FLOAT;
	};
	if (eager && ea.sum) {
		int s;
		idx_t c;
		ColumnBinding b;
		if (!SideOf(*ea.arg, gl, gr, s, c, b)) {
			return false;
		}
		const idx_t side = s == sx ? 0 : 1;
		has_w[side] = true;
		vb[side] = b;
		vt[side] = ea.arg->GetReturnType();
		if (!IsExactWeight(vt[side]) && vt[side].id() != LogicalTypeId::DOUBLE && vt[side].id() != LogicalTypeId::FLOAT) {
			return false;
		}
		info.has_value = true;
		info.eager = true;
		info.prod_type = vt[side];
		info.agg_type = IsIntegerKey(vt[side])      ? LogicalType::HUGEINT
		                : IsDecimalWeight(vt[side]) ? LogicalType::DECIMAL(38, DecimalType::GetScale(vt[side]))
		                                            : LogicalType::DOUBLE;
	} else if (eager && ea.arg) {
		int s;
		idx_t c;
		ColumnBinding b;
		if (!SideOf(*ea.arg, gl, gr, s, c, b) || !NoNulls(context, s == 0 ? gl : gr, c)) {
			return false;
		}
		info.has_value = false;
		info.eager = true;
	} else if (eager || info.distinct_keys || (ae.Function().GetName() == "count_star" && ae.GetChildren().empty())) {
		info.has_value = false;
		info.eager = eager;
	} else if ((ae.Function().GetName() == "sum" || ae.Function().GetName() == "avg") && arg &&
	           arg->GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
	           arg->Cast<BoundFunctionExpression>().Function().GetName() == "*") {
		auto &mul = arg->Cast<BoundFunctionExpression>();
		if (mul.GetChildren().size() != 2 || !weight(*mul.GetChildren()[0], below) || !weight(*mul.GetChildren()[1], below)) {
			return false;
		}
		info.has_value = true;
		info.prod_type = mul.GetReturnType();
	} else if ((ae.Function().GetName() == "sum" || ae.Function().GetName() == "avg") && ae.GetChildren().size() == 1) {
		if (!weight(*ae.GetChildren()[0], projs)) {
			return false;
		}
		info.has_value = true;
		info.prod_type = vt[has_w[0] ? 0 : 1];
	} else {
		return false;
	}
	if (info.has_value) {
		const idx_t one = has_w[0] ? (has_w[1] ? 2 : 1) : 0;
		if (one < 2) {
			vt[one] = IsDecimalWeight(vt[1 - one]) ? LogicalType::BIGINT : vt[1 - one];
		}
		info.v_int = IsExactWeight(vt[0]) && IsExactWeight(vt[1]);
	}
	auto at = info.agg_type.InternalType();
	if (at != PhysicalType::INT64 && at != PhysicalType::INT128 && at != PhysicalType::DOUBLE) {
		return false;
	}
	if (eager && ea.avg) {
		const auto ai = ea.avg_input.InternalType();
		if (agg.expressions.size() != 1 ||
		    (info.v_int ? ai != PhysicalType::INT16 && ai != PhysicalType::INT32 && ai != PhysicalType::INT64 &&
		                      ai != PhysicalType::INT128
		                : ai != PhysicalType::DOUBLE)) {
			return false;
		}
		info.two = true;
		info.aggs = {2};
		info.avg_input = ea.avg_input;
		info.agg2_type = info.agg_type;
		info.agg_type = LogicalType::BIGINT;
		info.count_first = false;
	} else if (info.two) {
		if (!info.has_value || info.distinct_keys) {
			return false;
		}
		idx_t ci = DConstants::INVALID_INDEX;
		for (idx_t i = 0; i < agg.expressions.size(); i++) {
			if (i != main_agg && (eager || (!agg_is(i, "sum") && !agg_is(i, "avg")))) {
				ci = i;
			}
		}
		auto &ce = agg.expressions[ci == DConstants::INVALID_INDEX ? main_agg : ci]->Cast<BoundAggregateExpression>();
		const auto &cn = ce.Function().GetName();
		if (ci == DConstants::INVALID_INDEX) {
			;
		} else if (ce.GetFilter() || ce.GetOrderBys() || (!eager && ce.GetReturnType().id() != LogicalTypeId::BIGINT)) {
			return false;
		} else if (eager) {
			int s;
			idx_t c;
			ColumnBinding b;
			if (ea_count.arg && (!SideOf(*ea_count.arg, gl, gr, s, c, b) || !NoNulls(context, s == 0 ? gl : gr, c))) {
				return false;
			}
		} else if (cn == "count_star" && ce.GetChildren().empty() && !ce.IsDistinct()) {
			;
		} else if ((cn == "count" || IsDistinctMarker(ce)) && ce.GetChildren().size() == 1) {
			int64_t off = 0;
			shared_ptr<Expression> str_compress;
			auto ve = ToScan(ThroughProjections(*ce.GetChildren()[0], projs, &off, &str_compress), scan_l, scan_r, true);
			int s;
			idx_t c;
			ColumnBinding b;
			if (!ve || !SideOf(*ve, gl, gr, s, c, b)) {
				return false;
			}
			if (ce.IsDistinct() || IsDistinctMarker(ce)) {
				if (info.nk != 1 || kxe[0] || kye[0] || !(b == kx[0] || b == ky[0])) {
					return false;
				}
				info.distinct_keys = true;
			} else if (!NoNulls(context, s == 0 ? gl : gr, c)) {
				return false;
			}
		} else {
			return false;
		}
		info.agg2_type = info.agg_type;
		if (navg) {
			if (info.distinct_keys) {
				return false;
			}
			for (idx_t i = 0; i < agg.expressions.size(); i++) {
				info.aggs.push_back(agg_is(i, "sum") ? 1 : agg_is(i, "avg") ? 2 : 0);
				if (agg_is(i, "sum")) {
					info.agg2_type = agg.expressions[i]->GetReturnType();
				} else if (agg_is(i, "avg")) {
					info.avg_input = agg.expressions[i]->Cast<BoundAggregateExpression>().GetChildren()[0]->GetReturnType();
				}
			}
			if (!nsum) {
				info.agg2_type = !info.v_int                     ? LogicalType::DOUBLE
				                 : IsDecimalWeight(info.prod_type) ? LogicalType::DECIMAL(38, DecimalType::GetScale(info.prod_type))
				                                                   : LogicalType::HUGEINT;
			}
			const auto ai = info.avg_input.InternalType();
			if (info.v_int ? ai != PhysicalType::INT16 && ai != PhysicalType::INT32 && ai != PhysicalType::INT64 &&
			                     ai != PhysicalType::INT128
			               : ai != PhysicalType::DOUBLE) {
				return false;
			}
			info.count_first = info.aggs[0] == 0;
		}
		info.agg_type = LogicalType::BIGINT;
	}
	if (dist) {
		vector<ColumnBinding> gb;
		for (auto &g : groups) {
			if (g.expr) {
				return false;
			}
			gb.push_back(g.b);
		}
		if (info.has_value || info.eager || info.distinct_keys || info.nk != 1 || groups.size() != 2 || !gl.get || !gr.get ||
		    !DistinctGroups(*dist, projs, dist_below, scan_l, scan_r, gb, {kx[0], ky[0]})) {
			return false;
		}
		info.distinct_keys = true;
	}
	bool nullable = false;
	for (idx_t g = 0; g < groups.size(); g++) {
		auto &col = (info.out_groups[g].first == 0 ? info.xcols : info.ycols)[info.out_groups[g].second];
		if (!groups[g].expr && !NoNulls(context, groups[g].side == 0 ? gl : gr, groups[g].base)) {
			if ((col.compress && !groups[g].text) || col.off != 0) {
				return false;
			}
			col.nullable = true;
		}
		nullable = nullable || col.nullable || groups[g].text;
	}
	for (idx_t s = 0; s < 2 && info.has_value; s++) {
		if (has_w[s] && !NoNulls(context, s == 0 ? gx : gy, InputColumn(s == 0 ? gx : gy, vb[s]))) {
			return false;
		}
	}
	double expansion = -1;
	if (gx.get && gy.get && !kxe[0] && !kye[0]) {
		expansion = Expansion(context, *gx.get, BaseColumn(*gx.get, kx[0]), gy.get, BaseColumn(*gy.get, ky[0]));
	} else if (info.nk == 1 && !kxe[0] && !kye[0]) {
		double na, va, nb, vb;
		if (InputStats(context, gx, kx[0], na, va) && InputStats(context, gy, ky[0], nb, vb) && na + nb > 0) {
			expansion = na * nb / (std::max(va, vb) * (na + nb));
		}
	}
	if (expansion < 0 && join.has_estimated_cardinality && join.children[0]->has_estimated_cardinality &&
	    join.children[1]->has_estimated_cardinality &&
	    join.children[0]->estimated_cardinality + join.children[1]->estimated_cardinality > 0) {
		expansion = (double)join.estimated_cardinality /
		            (double)(join.children[0]->estimated_cardinality + join.children[1]->estimated_cardinality);
	}
	if (!JoinExpands(context, expansion, true)) {
		return false;
	}
	info.general_cross =
	    info.nk > 1 || info.xcols.size() > 1 || info.ycols.size() > 1 || !info.filters.empty() || info.two || nullable;
	{
		vector<ColumnBinding> xb, yb;
		bool computed = false;
		for (idx_t g = 0; g < groups.size(); g++) {
			(info.out_groups[g].first == 0 ? xb : yb).push_back(groups[g].b);
			computed = computed || groups[g].expr;
		}
		auto same = [&](ColumnBinding a, ColumnBinding b) {
			return InputColumn(gx, a) == InputColumn(gy, b);
		};
		bool sym = info.general_cross && !computed && gx.get && gy.get && gx.get->GetTable() &&
		           gx.get->GetTable().get() == gy.get->GetTable().get() &&
		           SameTableFilters(*gx.get, *gy.get) && !scan_l.filter &&
		           !scan_r.filter && !gx.get->extra_info.sample_options && !gy.get->extra_info.sample_options && !eager &&
		           xb.size() == yb.size() &&
		           !vcast[0] && !vcast[1] &&
		           (!info.has_value || (has_w[0] && has_w[1] && same(vb[0], vb[1])));
		for (idx_t c = 0; sym && c < xb.size(); c++) {
			sym = same(xb[c], yb[c]);
		}
		for (idx_t c = 0; sym && c < info.nk; c++) {
			sym = !kxe[c] && !kye[c] && same(kx[c], ky[c]);
		}
		info.symmetric = sym;
	}
	info.x_type = agg.groups[0]->GetReturnType();
	info.y_type = info.out_groups.size() > 1 ? agg.groups[1]->GetReturnType() : info.x_type;
	info.x_str = info.xcols[0].str;
	info.y_str = info.ycols[0].str;
	info.x_off[0] = info.xcols[0].off;
	info.x_off[1] = info.ycols[0].off;
	info.x_compress[0] = info.xcols[0].compress;
	info.x_compress[1] = info.ycols[0].compress;
	auto colref = [](const LogicalType &t, ColumnBinding b) -> unique_ptr<Expression> {
		return make_uniq<BoundColumnRefExpression>(t, b);
	};
	auto null_of = [](const LogicalType &t) -> unique_ptr<Expression> {
		return make_uniq<BoundConstantExpression>(Value(t));
	};
	auto side_rows = [&](unique_ptr<LogicalOperator> get, idx_t side) -> unique_ptr<LogicalOperator> {
		vector<unique_ptr<Expression>> e;
		e.push_back(make_uniq<BoundConstantExpression>(Value::INTEGER((int32_t)side)));
		for (idx_t c = 0; c < info.nk; c++) {
			const LogicalType kt =
			    info.k_strs[c] ? LogicalType::VARCHAR : IsTemporalKey(kxt[c]) ? kxt[c] : LogicalType::BIGINT;
			auto &ke = side ? kye[c] : kxe[c];
			e.push_back(BoundCastExpression::AddCastToType(
			    context, ke ? ke->Copy() : colref(side ? kyt[c] : kxt[c], side ? ky[c] : kx[c]), kt));
		}
		for (idx_t s = 0; s < 2; s++) {
			for (idx_t g = 0; g < groups.size(); g++) {
				if (info.out_groups[g].first != s) {
					continue;
				}
				if (groups[g].text) {
					e.push_back(s != side ? null_of(LogicalType::VARCHAR)
					                      : BoundCastExpression::AddCastToType(
					                            context, groups[g].expr ? groups[g].expr->Copy() : colref(groups[g].type, groups[g].b),
					                            LogicalType::VARCHAR));
					continue;
				}
				e.push_back(s != side         ? null_of(groups[g].type)
				            : groups[g].expr ? groups[g].expr->Copy()
				                             : colref(groups[g].type, groups[g].b));
			}
		}
		if (info.has_value) {
			for (idx_t s = 0; s < 2; s++) {
				const LogicalType vt_u = !info.v_int                        ? LogicalType::DOUBLE
				                         : has_w[s] && IsDecimalWeight(vt[s]) ? vt[s]
				                                                              : LogicalType::BIGINT;
				if (s != side) {
					e.push_back(null_of(vt_u));
				} else if (has_w[s]) {
					e.push_back(BoundCastExpression::AddCastToType(context, vcast[s] ? vcast[s]->Copy() : colref(vt[s], vb[s]), vt_u));
				} else {
					e.push_back(make_uniq<BoundConstantExpression>(Value(1).DefaultCastAs(vt_u)));
				}
			}
		}
		auto proj = make_uniq<LogicalProjection>(binder.GenerateTableIndex(), std::move(e));
		proj->children.push_back(std::move(get));
		return std::move(proj);
	};
	auto join_owned = std::move(*slot);
	auto agg_owned = unique_ptr_cast<LogicalOperator, LogicalAggregate>(std::move(plan));
	agg_owned->children.clear();
	auto first = gx.get ? TakeScan(join_owned->children[sx]) : TakeBelow(join_owned->children[sx], (sx == 0 ? scan_l : scan_r).depth);
	auto second = gy.get ? TakeScan(join_owned->children[1 - sx])
	                     : TakeBelow(join_owned->children[1 - sx], (sx == 0 ? scan_r : scan_l).depth);
	unique_ptr<LogicalOperator> all;
	if (info.symmetric) {
		first->Cast<LogicalGet>().dynamic_filters = nullptr;
		all = side_rows(std::move(first), 0);
	} else {
		const idx_t ncols = 1 + info.nk + groups.size() + (info.has_value ? 2 : 0);
		all = make_uniq<LogicalSetOperation>(binder.GenerateTableIndex(), ncols, side_rows(std::move(first), 0),
		                                     side_rows(std::move(second), 1), LogicalOperatorType::LOGICAL_UNION, true);
	}
	all->ResolveOperatorTypes();
	if (info.eager) {
		if (!info.aggs.empty()) {
			for (idx_t i = 0; i < info.aggs.size(); i++) {
				agg_owned->expressions[i] = make_uniq<BoundConstantExpression>(
				    Value(info.aggs[i] == 0 ? LogicalType::BIGINT : info.aggs[i] == 1 ? info.agg2_type : LogicalType::DOUBLE));
			}
		} else if (info.two) {
			agg_owned->expressions[info.count_first ? 0 : 1] = make_uniq<BoundConstantExpression>(Value(info.agg_type));
			agg_owned->expressions[info.count_first ? 1 : 0] = make_uniq<BoundConstantExpression>(Value(info.agg2_type));
		} else {
			agg_owned->expressions[0] = make_uniq<BoundConstantExpression>(Value(info.agg_type));
		}
	}
	const bool prune = !info.two || info.count_first;
	info.min_value = prune ? push.min_value : 0;
	info.topk = prune && info.filters.empty() ? push.topk : -1;
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

template <class F>
static bool Attempt(F &&f) {
	try {
		return f();
	} catch (InterruptException &) {
		throw;
	} catch (std::exception &) {
		return false;
	}
}

static void Inject(ClientContext &context, unique_ptr<LogicalOperator> &plan, Binder &binder) {
	if (!plan) {
		return;
	}
	if (plan->type == LogicalOperatorType::LOGICAL_DISTINCT && Attempt([&] { return TryTesseraDistinct(context, plan); })) {
		return;
	}
	if (plan->type == LogicalOperatorType::LOGICAL_FILTER || plan->type == LogicalOperatorType::LOGICAL_TOP_N) {
		TesseraPushdown push;
		auto slot = plan->type == LogicalOperatorType::LOGICAL_FILTER ? HavingBelow(plan->Cast<LogicalFilter>(), push)
		                                                              : TopNBelow(plan->Cast<LogicalTopN>(), push);
		bool limited = false;
		if (slot && (Attempt([&] { return TryTessera(context, *slot, push, &limited); }) ||
		             (!limited && (Attempt([&] { return TryTesseraCross(context, *slot, push, binder); }) ||
		                           Attempt([&] { return TryTesseraCross(context, *slot, push, binder, true); }))))) {
			return;
		}
	}
	bool limited = false;
	if (plan->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY &&
	    (Attempt([&] { return TryTessera(context, plan, TesseraPushdown(), &limited); }) ||
	     (!limited && (Attempt([&] { return TryTesseraCross(context, plan, TesseraPushdown(), binder); }) ||
	                   Attempt([&] { return TryTesseraCross(context, plan, TesseraPushdown(), binder, true); }))))) {
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
	} else if (md == "ne") {
		info.mode = TesseraPairMode::NE;
	} else {
		throw BinderException("tessera_sim: mode is 'lt', 'le', 'full' or 'ne'");
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
	if (!info.k_str && !IsOrderedKey(kt)) {
		throw BinderException("tessera_sim: k must be an integer or VARCHAR column");
	}
	if (!info.x_str && !IsOrderedKey(xt)) {
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

static void FinishEagerCounts(ClientContext &context, LogicalOperator &op, column_binding_map_t<LogicalType> &carried) {
	for (auto &child : op.children) {
		FinishEagerCounts(context, *child, carried);
	}
	if (op.type == LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR) {
		auto &ext = op.Cast<LogicalExtensionOperator>();
		if (ext.GetExtensionName() == "tessera" && ext.Cast<LogicalTessera>().info.eager) {
			auto &t = ext.Cast<LogicalTessera>();
			for (idx_t i = 0; i < t.agg_op->expressions.size(); i++) {
				const bool count = !t.info.two || (i == 0) == t.info.count_first;
				carried[ColumnBinding(t.agg_op->aggregate_index, ProjectionIndex(i))] =
				    !t.info.aggs.empty() ? (t.info.aggs[i] == 0   ? LogicalType::BIGINT
				                            : t.info.aggs[i] == 1 ? t.info.agg2_type
				                                                  : LogicalType::DOUBLE)
				    : count              ? t.info.agg_type
				                         : t.info.agg2_type;
			}
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
	auto type_of = [&](const Expression &e) {
		return carried.find(e.Cast<BoundColumnRefExpression>().Binding())->second;
	};
	std::function<void(unique_ptr<Expression> &)> finish = [&](unique_ptr<Expression> &e) {
		if (IsFinalize(*e) && carries(*e->Cast<BoundFunctionExpression>().GetChildren()[0])) {
			auto type = e->GetReturnType();
			auto value = std::move(e->Cast<BoundFunctionExpression>().GetChildrenMutable()[0]);
			value->SetReturnType(type_of(*value));
			e = BoundCastExpression::AddCastToType(context, std::move(value), type);
			return;
		}
		ExpressionIterator::EnumerateChildren(*e, finish);
	};
	for (idx_t i = 0; i < proj.expressions.size(); i++) {
		auto &e = proj.expressions[i];
		if (carries(*e)) {
			const LogicalType t = type_of(*e);
			e->SetReturnType(t);
			carried[ColumnBinding(proj.table_index, ProjectionIndex(i))] = t;
		} else {
			finish(e);
		}
	}
}

struct DistinctMarker : public FunctionData {
	explicit DistinctMarker(BoundAggregateFunction f) : original(std::move(f)) {
	}
	BoundAggregateFunction original;
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<DistinctMarker>(original);
	}
	bool Equals(const FunctionData &other) const override {
		return original == other.Cast<DistinctMarker>().original;
	}
};

[[noreturn]] static void MarkerRan() {
	throw InternalException("tessera: a COUNT(DISTINCT) marker reached execution");
}
static idx_t MarkerSize(AggregateStateInput &) {
	return sizeof(int64_t);
}
static void MarkerInit(AggregateStateInput &, data_ptr_t *, idx_t) {
}
static void MarkerUpdate(Vector[], AggregateInputData &, idx_t, Vector &, idx_t) {
	MarkerRan();
}
static void MarkerCombine(Vector &, Vector &, AggregateInputData &, idx_t) {
	MarkerRan();
}
static void MarkerFinalize(Vector &, AggregateFinalizeInputData &, Vector &, idx_t, idx_t) {
	MarkerRan();
}

static bool IsDistinctMarker(const BoundAggregateExpression &a) {
	return a.Function().GetName() == "tessera_count_distinct" && a.BindInfo();
}

static bool ExpandingJoinBelow(ClientContext &context, LogicalOperator &op) {
	vector<Expression *> conds;
	LogicalOperator *cur = &op;
	while ((cur->type == LogicalOperatorType::LOGICAL_PROJECTION || cur->type == LogicalOperatorType::LOGICAL_FILTER) &&
	       cur->children.size() == 1) {
		if (cur->type == LogicalOperatorType::LOGICAL_FILTER) {
			for (auto &e : cur->expressions) {
				conds.push_back(e.get());
			}
		}
		cur = cur->children[0].get();
	}
	if ((cur->type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN && cur->type != LogicalOperatorType::LOGICAL_CROSS_PRODUCT) ||
	    cur->children.size() != 2) {
		return false;
	}
	LogicalGet *gets[2];
	for (idx_t s = 0; s < 2; s++) {
		LogicalOperator *c = cur->children[s].get();
		while ((c->type == LogicalOperatorType::LOGICAL_PROJECTION || c->type == LogicalOperatorType::LOGICAL_FILTER) &&
		       c->children.size() == 1) {
			c = c->children[0].get();
		}
		if (c->type != LogicalOperatorType::LOGICAL_GET || !c->Cast<LogicalGet>().GetTable()) {
			return false;
		}
		gets[s] = &c->Cast<LogicalGet>();
	}
	auto expands = [&](Expression &l, Expression &r) {
		int sl, sr;
		idx_t cl, cr;
		ColumnBinding bl, br;
		if (!SideOf(l, *gets[0], *gets[1], sl, cl, bl) || !SideOf(r, *gets[0], *gets[1], sr, cr, br) || sl == sr) {
			return false;
		}
		LogicalGet &ga = sl == 0 ? *gets[0] : *gets[1], &gb = sl == 0 ? *gets[1] : *gets[0];
		return JoinExpands(context, Expansion(context, ga, cl, &gb, cr), true);
	};
	if (cur->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		for (auto &c : cur->Cast<LogicalComparisonJoin>().conditions) {
			if (c.GetComparisonType() == ExpressionType::COMPARE_EQUAL) {
				return expands(c.GetLHS(), c.GetRHS());
			}
		}
	}
	for (auto *e : conds) {
		if (e->GetExpressionType() == ExpressionType::COMPARE_EQUAL && BoundComparisonExpression::IsComparison(*e)) {
			auto &c = e->Cast<BoundFunctionExpression>();
			return expands(const_cast<Expression &>(BoundComparisonExpression::Left(c)),
			               const_cast<Expression &>(BoundComparisonExpression::Right(c)));
		}
	}
	return false;
}

static void MarkDistinct(ClientContext &context, unique_ptr<LogicalOperator> &op) {
	for (auto &c : op->children) {
		MarkDistinct(context, c);
	}
	if (op->type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY || op->children.size() != 1) {
		return;
	}
	auto &agg = op->Cast<LogicalAggregate>();
	if (agg.groups.size() < 2 || agg.grouping_sets.size() > 1 || agg.expressions.size() < 2) {
		return;
	}
	idx_t nd = 0, di = 0;
	for (idx_t i = 0; i < agg.expressions.size(); i++) {
		if (agg.expressions[i]->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
			return;
		}
		if (agg.expressions[i]->Cast<BoundAggregateExpression>().IsDistinct()) {
			nd++;
			di = i;
		}
	}
	if (nd != 1) {
		return;
	}
	auto &d = agg.expressions[di]->Cast<BoundAggregateExpression>();
	if (!(d.Function().GetName() == "count") || d.GetChildren().size() != 1 || d.GetFilter() || d.GetOrderBys() ||
	    !ExpandingJoinBelow(context, *agg.children[0])) {
		return;
	}
	AggregateFunction fn(Identifier("tessera_count_distinct"), {d.GetChildren()[0]->GetReturnType()}, d.GetReturnType(),
	                     MarkerSize, MarkerInit, MarkerUpdate, MarkerCombine, MarkerFinalize,
	                     FunctionNullHandling::DEFAULT_NULL_HANDLING);
	auto marker = make_uniq<BoundAggregateExpression>(BoundAggregateFunction(fn), std::move(d.GetChildrenMutable()), nullptr,
	                                                  make_uniq<DistinctMarker>(d.Function()), AggregateType::NON_DISTINCT);
	marker->SetAlias(d.GetAlias());
	agg.expressions[di] = std::move(marker);
}

static void RestoreDistinct(LogicalOperator &op) {
	for (auto &c : op.children) {
		RestoreDistinct(*c);
	}
	vector<unique_ptr<Expression>> *exprs = nullptr;
	if (op.type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		exprs = &op.Cast<LogicalAggregate>().expressions;
	} else if (op.type == LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR &&
	           op.Cast<LogicalExtensionOperator>().GetExtensionName() == "tessera") {
		exprs = &op.Cast<LogicalTessera>().agg_op->expressions;
	}
	for (idx_t i = 0; exprs && i < exprs->size(); i++) {
		auto &e = (*exprs)[i];
		if (e->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE || !IsDistinctMarker(e->Cast<BoundAggregateExpression>())) {
			continue;
		}
		auto &m = e->Cast<BoundAggregateExpression>();
		auto r = make_uniq<BoundAggregateExpression>(m.BindInfo()->Cast<DistinctMarker>().original,
		                                             std::move(m.GetChildrenMutable()), std::move(m.GetFilterMutable()),
		                                             nullptr, AggregateType::DISTINCT);
		r->SetAlias(m.GetAlias());
		e = std::move(r);
	}
}

static bool TesseraOn(ClientContext &context) {
	Value enabled;
	return tessera::cpu_supported() && !(context.TryGetCurrentSetting("tessera_enabled", enabled) && !enabled.IsNull() &&
	                                     !enabled.GetValue<bool>());
}

static void TesseraPreOptimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	if (TesseraOn(input.context)) {
		Attempt([&] {
			MarkDistinct(input.context, plan);
			return true;
		});
	}
}

static void TesseraOptimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	if (!TesseraOn(input.context)) {
		return;
	}
	Inject(input.context, plan, input.optimizer.binder);
	RestoreDistinct(*plan);
	column_binding_map_t<LogicalType> carried;
	FinishEagerCounts(input.context, *plan, carried);
	if (!carried.empty()) {
		plan->ResolveOperatorTypes();
	}
}

static void LoadInternal(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	OptimizerExtension ext;
	ext.optimize_function = TesseraOptimize;
	ext.pre_optimize_function = TesseraPreOptimize;
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
