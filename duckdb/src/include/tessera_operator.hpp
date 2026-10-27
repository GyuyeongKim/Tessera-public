#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"

namespace duckdb {

enum class TesseraPairMode : uint8_t {
	FULL,
	LT,
	LE
};

struct TesseraPlanInfo {
	ColumnBinding k_bind, x_bind, v_bind;
	bool has_value = false;
	bool distinct_only = false;
	idx_t k_pos = 0, x_pos = 0, v_pos = DConstants::INVALID_INDEX;
	TesseraPairMode mode = TesseraPairMode::FULL;
	bool col0_smaller = true;
	int64_t x_off[2] = {0, 0};
	bool k_str = false, x_str = false;
	shared_ptr<Expression> x_compress[2];
	bool cross = false, y_str = false;
	LogicalType y_type;
	uint64_t min_value = 0;
	int64_t topk = -1;
	int sim_measure = 0;
	double min_sim = 0;
	LogicalType x_type, v_type, agg_type;
	bool v_int = false;
	LogicalType prod_type;
};

class LogicalTessera : public LogicalExtensionOperator {
public:
	LogicalTessera(unique_ptr<LogicalAggregate> agg_op, unique_ptr<LogicalOperator> child, TesseraPlanInfo info);

	unique_ptr<LogicalAggregate> agg_op;
	TesseraPlanInfo info;

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
	PhysicalTessera(vector<LogicalType> types, PhysicalOperator &child, TesseraPlanInfo info, idx_t estimated_cardinality);

	TesseraPlanInfo info;

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
	SourceResultType GetData(ExecutionContext &context, DataChunk &chunk, OperatorSourceInput &input) const override;
	bool IsSource() const override {
		return true;
	}
	bool ParallelSource() const override {
		return true;
	}
};

}
