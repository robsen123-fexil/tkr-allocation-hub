#include "tkr/desk/desk_registry.h"

#include "tkr/desk/allocation_solver.h"
#include "tkr/desk/benchmark_tracker.h"
#include "tkr/desk/exposure_aggregator.h"
#include "tkr/desk/nav_calculator.h"
#include "tkr/desk/allocation_reconciler.h"
#include "tkr/desk/order_blotter.h"
#include "tkr/desk/portfolio_snapshot.h"
#include "tkr/desk/compliance_rule_engine.h"
#include "tkr/desk/corporate_action_adjuster.h"
#include "tkr/desk/counterparty_limit.h"
#include "tkr/desk/fee_accrual_engine.h"
#include "tkr/desk/haircut_calculator.h"
#include "tkr/desk/lot_splitter.h"
#include "tkr/desk/margin_aggregator.h"
#include "tkr/desk/position_ledger.h"
#include "tkr/desk/pro_rata_allocator.h"
#include "tkr/desk/restriction_filter.h"
#include "tkr/desk/settlement_calendar.h"
#include "tkr/desk/tax_lot_matcher.h"
#include "tkr/ledger/audit_spool.h"

namespace tkr {
namespace desk {

Status DeskRegistry::RunCompliancePass(const BatchWireFrame& frame) {
  RestrictionFilter filter(RestrictionFilterConfig{frame.header.desk_id, true});
  RestrictionFilterResult restricted = filter.Filter(frame);
  if (restricted.status == Status::kComplianceReject) {
    return Status::kComplianceReject;
  }

  ComplianceRuleEngine engine(
      ComplianceRuleEngineConfig{frame.header.desk_id, true});
  ComplianceRuleEngineResult res = engine.Evaluate(frame);
  if (res.status != Status::kOk) {
    return res.status;
  }
  if (res.rejected_records > 0) {
    return Status::kComplianceReject;
  }
  return Status::kOk;
}

Status DeskRegistry::RunMarginPass(const BatchWireFrame& frame) {
  MarginAggregator agg(MarginAggregatorConfig{frame.header.desk_id, 1000});
  MarginAggregatorResult res = agg.Aggregate(frame);
  if (res.status != Status::kOk && res.status != Status::kMarginBreach) {
    return res.status;
  }
  if (res.breach_count > 0) {
    return Status::kMarginBreach;
  }
  return Status::kOk;
}

Status DeskRegistry::RunBatchDesks(BatchWireFrame& frame, DeskRunContext* ctx) {
  if (ctx == nullptr) {
    return Status::kBoundsError;
  }

  ctx->batch_id = frame.header.desk_id;
  ctx->record_count = static_cast<std::uint32_t>(frame.records.size());
  ctx->total_qty_milli = 0;
  for (const WireBatchRecord& rec : frame.records) {
    ctx->total_qty_milli += rec.qty_milli;
  }

  Status compliance = RunCompliancePass(frame);
  ctx->compliance_ok = compliance == Status::kOk;
  if ((frame.header.flags & kBatchFlagComplianceHold) != 0 &&
      !ctx->compliance_ok) {
    return compliance;
  }

  CounterpartyLimit cp_limit(CounterpartyLimitConfig{frame.header.desk_id, true});
  CounterpartyLimitResult cp_result = cp_limit.Evaluate(frame);
  if (cp_result.status == Status::kComplianceReject) {
    ctx->compliance_ok = false;
    return Status::kComplianceReject;
  }

  SettlementCalendar calendar(SettlementCalendarConfig{
      MarketRegion::kUsEquity, SettlementCycle::kT2, true});
  SettlementCalendarResult settle_result = calendar.ComputeBatch(frame);
  if (settle_result.status != Status::kOk) {
    return settle_result.status;
  }

  LotSplitter splitter(LotSplitterConfig{1000, true});
  LotSplitterResult split = splitter.SplitBatch(frame, LotSplitPolicy::kProRataByWeight);
  if (split.status != Status::kOk) {
    return split.status;
  }

  ProRataAllocator allocator(ProRataAllocatorConfig{true});
  ProRataAllocatorResult alloc = allocator.Allocate(frame);
  if (alloc.status != Status::kOk) {
    return alloc.status;
  }

  AllocationSolver solver(AllocationSolverConfig{true, 1000000});
  AllocationSolverResult solved = solver.Solve(frame, alloc.slices);
  if (solved.status != Status::kOk) {
    return solved.status;
  }

  PositionLedger ledger(PositionLedgerConfig{frame.header.desk_id});
  Status ledger_st = ledger.ApplyBatch(frame, solved.final_slices);
  if (ledger_st != Status::kOk) {
    return ledger_st;
  }

  FeeAccrualEngine fees(FeeAccrualEngineConfig{frame.header.desk_id, frame.header.trade_date_yyyymmdd});
  FeeAccrualEngineResult fee_res = fees.AccrueBatch(frame);
  if (fee_res.status != Status::kOk) {
    return fee_res.status;
  }

  TaxLotMatcher matcher(TaxLotMatcherConfig{TaxLotMatchMethod::kFifo});
  for (const WireBatchRecord& rec : frame.records) {
    TaxLot lot{};
    lot.lot_id = rec.record_id;
    lot.account_id = rec.account_id;
    lot.symbol_id = rec.symbol_id;
    lot.qty_milli = static_cast<std::int64_t>(rec.qty_milli);
    lot.cost_basis_cents =
        (static_cast<std::int64_t>(rec.qty_milli) *
         static_cast<std::int64_t>(rec.price_tick)) /
        1000;
    lot.acquire_date_yyyymmdd = frame.header.trade_date_yyyymmdd;
    lot.closed = false;
    matcher.OpenLot(lot);
  }

  BenchmarkTracker tracker(BenchmarkTrackerConfig{1, 100});
  BenchmarkTrackerResult bench = tracker.TrackBatch(frame);

  NavCalculator nav(25, 150);
  NavSnapshot nav_snap = nav.Compute(frame);
  ExposureAggregator exposure(frame.header.desk_id, true);
  ExposureReport exposure_report = exposure.Aggregate(frame);
  PortfolioSnapshotBuilder portfolio(frame.header.desk_id, 100000000);
  PortfolioSnapshot portfolio_snap = portfolio.Build(frame);
  OrderBlotter blotter(frame.header.trade_date_yyyymmdd);
  blotter.IngestBatch(frame);
  AllocationReconciler reconciler;
  ReconcileReport reconcile_report = reconciler.ReconcileBatch(frame, alloc.slices);

  Status margin = RunMarginPass(frame);
  ctx->margin_ok = margin == Status::kOk;
  if ((frame.header.flags & kBatchFlagMarginCheck) != 0 && !ctx->margin_ok) {
    return margin;
  }

  AllocationBatchSummary summary{};
  summary.batch_id = frame.header.desk_id;
  summary.record_count = ctx->record_count;
  summary.total_qty_milli = ctx->total_qty_milli;
  summary.desk_id = frame.header.desk_id;
  summary.flags = frame.header.flags;
  summary.margin_cleared = ctx->margin_ok;
  summary.compliance_cleared = ctx->compliance_ok;
  ledger::GlobalAuditSpool().AppendBatchSummary(summary);

  (void)bench;
  (void)split;
  (void)settle_result;
  (void)nav_snap;
  (void)exposure_report;
  (void)portfolio_snap;
  (void)blotter;
  (void)reconcile_report;
  return Status::kOk;
}

Status DeskRegistry::RunHaircutPass(const BatchWireFrame& frame) {
  HaircutCalculator calc(HaircutCalculatorConfig{frame.header.desk_id, true});
  std::vector<HaircutInput> inputs;
  inputs.reserve(frame.records.size());

  std::int64_t total_gross = 0;
  for (const WireBatchRecord& rec : frame.records) {
    total_gross +=
        (static_cast<std::int64_t>(rec.qty_milli) *
         static_cast<std::int64_t>(rec.price_tick)) /
        1000;
  }

  for (const WireBatchRecord& rec : frame.records) {
    HaircutInput input{};
    input.symbol_id = rec.symbol_id;
    input.asset_class = AssetClass::kEquity;
    input.rating = CreditRating::kA;
    input.market_value_cents =
        (static_cast<std::int64_t>(rec.qty_milli) *
         static_cast<std::int64_t>(rec.price_tick)) /
        1000;
    if (total_gross > 0) {
      input.portfolio_weight_bp = static_cast<std::uint32_t>(
          (input.market_value_cents * 10000) / total_gross);
    }
    inputs.push_back(input);
  }

  HaircutCalculatorSummary summary = calc.Compute(inputs);
  return summary.status;
}

Status DeskRegistry::RunCorporateActionPass(const BatchWireFrame& frame) {
  CorporateActionAdjuster adjuster(
      CorporateActionAdjusterConfig{frame.header.desk_id});
  CorporateActionAdjusterResult result = adjuster.ApplyToBatch(frame);
  return result.status;
}

}  // namespace desk
}  // namespace tkr
