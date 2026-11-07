#pragma once

#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"

namespace duckdb {

enum class TesseraGroupOf : uint8_t { EACH, LOWER, HIGHER, FIRST, SECOND };

enum class TesseraPairMode : uint8_t {
	FULL,
	LT,
	LE,
	NE
};

struct TesseraSum {
	enum Kind : uint8_t { SUM, AVG, COUNT, HIDDEN };
	bool v_int = false;
	LogicalType sum_type, prod_type;
	idx_t factors[2] = {0, 0};
	Kind kind = SUM;
	bool ones = false;
	int64_t null_like = -1;
	idx_t div = 0;
	LogicalType avg_input;
};

struct TesseraPlanInfo {
	ColumnBinding k_bind, x_bind, v_bind;
	bool has_value = false;
	bool distinct_keys = false;
	bool distinct_only = false;
	bool eager = false;
	idx_t k_pos = 0, x_pos = 0, v_pos = DConstants::INVALID_INDEX;
	TesseraPairMode mode = TesseraPairMode::FULL;
	bool col0_smaller = true;
	int64_t x_off[2] = {0, 0};
	bool k_str = false, x_str = false;
	shared_ptr<Expression> x_compress[2];
	bool cross = false, y_str = false;
	LogicalType y_type;
	struct TupleCol {
		LogicalType type;
		bool str = false;
		bool nullable = false;
		int64_t off = 0;
		shared_ptr<Expression> compress;
	};
	struct PairFilter {
		idx_t xi, yi;
		ExpressionType op;
	};
	bool general_cross = false;
	idx_t nk = 1;
	vector<bool> k_strs;
	vector<TupleCol> xcols, ycols;
	vector<std::pair<uint8_t, idx_t>> out_groups;
	vector<PairFilter> filters;
	bool two = false, count_first = true;
	LogicalType agg2_type;
	vector<uint8_t> aggs;
	vector<TesseraSum> sums;
	bool sum_nulls = false;
	LogicalType avg_input;
	bool symmetric = false;
	bool KDict() const {
		return nk > 1 || k_str;
	}
	bool XDict() const {
		return general_cross ? xcols.size() > 1 || xcols[0].str || xcols[0].nullable : x_str;
	}
	bool YDict() const {
		return general_cross ? ycols.size() > 1 || ycols[0].str || ycols[0].nullable : y_str;
	}
	static bool Tuples(const vector<TupleCol> &cols) {
		return cols.size() > 1 || cols[0].nullable;
	}
	int ManyPairOp() const {
		if (sums.empty() || filters.size() != 1 || xcols.size() != 1 || ycols.size() != 1 || xcols[0].str || ycols[0].str ||
		    Tuples(xcols) || Tuples(ycols)) {
			return 0;
		}
		switch (filters[0].op) {
		case ExpressionType::COMPARE_LESSTHAN:
			return 1;
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			return 2;
		case ExpressionType::COMPARE_GREATERTHAN:
			return 3;
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			return 4;
		case ExpressionType::COMPARE_NOTEQUAL:
			return 5;
		default:
			return 0;
		}
	}
	uint64_t min_value = 0;
	int64_t topk = -1;
	int64_t group_topk = -1;
	TesseraGroupOf group_of = TesseraGroupOf::EACH;
	int sim_measure = 0;
	double min_sim = 0;
	LogicalType x_type, v_type, agg_type;
	bool v_int = false;
	LogicalType prod_type;
};

inline InsertionOrderPreservingMap<string> TesseraPruneParams(const TesseraPlanInfo &info) {
	InsertionOrderPreservingMap<string> r;
	if (info.min_value > 0) {
		r["Prune"] = ">= " + to_string(info.min_value);
	}
	if (info.topk > 0) {
		r["Prune"] = "top " + to_string(info.topk);
	}
	if (info.group_topk > 0) {
		r["Prune"] = "rank <= " + to_string(info.group_topk);
	}
	return r;
}

class LogicalTessera : public LogicalExtensionOperator {
public:
	LogicalTessera(unique_ptr<LogicalAggregate> agg_op, unique_ptr<LogicalOperator> child, TesseraPlanInfo info);

	unique_ptr<LogicalAggregate> agg_op;
	TesseraPlanInfo info;

	InsertionOrderPreservingMap<string> ParamsToString() const override {
		return TesseraPruneParams(info);
	}

	string GetName() const override {
		return "TESSERA";
	}
	string GetExtensionName() const override {
		return "tessera";
	}
	vector<ColumnBinding> GetColumnBindings() override;
	PhysicalOperator &CreatePlan(ClientContext &context, PhysicalPlanGenerator &generator) override;

protected:
	void ResolveTypes() override;
};

class PhysicalTessera : public PhysicalOperator {
public:
	PhysicalTessera(PhysicalPlan &physical_plan, vector<LogicalType> types, PhysicalOperator &child,
	                TesseraPlanInfo info, idx_t estimated_cardinality);

	TesseraPlanInfo info;

	InsertionOrderPreservingMap<string> ParamsToString() const override {
		return TesseraPruneParams(info);
	}

	string GetName() const override {
		return "TESSERA";
	}

	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override;
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                          OperatorSinkFinalizeInput &input) const override;
	bool IsSink() const override {
		return true;
	}
	bool ParallelSink() const override {
		return true;
	}
	bool SinkOrderDependent() const override {
		return false;
	}

	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override;
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;
	bool IsSource() const override {
		return true;
	}
	bool ParallelSource() const override {
		return true;
	}
};

}
