#include "tkr/desk/allocation_solver.h"
#include "tkr/desk/pro_rata_allocator.h"
#include "tkr/engine/pipeline_orchestrator.h"
#include "tkr/types.h"
#include "tkr/util/bounds.h"
#include "tkr/wire/batch_wire_codec.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void PrintUsage(const char* prog) {
  std::cerr << "Usage: " << prog << " <command> [args]\n"
            << "Commands:\n"
            << "  allocate <total_qty> <weight1> <weight2> ...  Pro-rata allocation\n"
            << "  pipeline <file>                               Run allocation pipeline\n"
            << "  stats                                         Show pipeline stats\n";
}

int CmdAllocate(int argc, char** argv) {
  if (argc < 4) {
    std::cerr << "allocate requires total_qty and at least one weight\n";
    return 1;
  }

  std::int64_t total_qty = 0;
  if (!tkr::util::ParseAsciiInt(argv[2], &total_qty) || total_qty <= 0) {
    std::cerr << "invalid total_qty\n";
    return 1;
  }

  std::vector<tkr::desk::ProRataWeight> weights;
  std::vector<tkr::WireBatchRecord> records;

  for (int i = 3; i < argc; ++i) {
    std::int64_t weight = 0;
    if (!tkr::util::ParseAsciiInt(argv[i], &weight) || weight <= 0) {
      std::cerr << "invalid weight: " << argv[i] << "\n";
      return 1;
    }

    tkr::desk::ProRataWeight w{};
    w.account_id = static_cast<std::uint32_t>(1000 + i);
    w.weight_bp = static_cast<std::uint32_t>(weight * 100);
    weights.push_back(w);

    tkr::WireBatchRecord rec{};
    rec.record_id = static_cast<std::uint32_t>(i);
    rec.account_id = w.account_id;
    rec.qty_milli = w.weight_bp;
    records.push_back(rec);
  }

  tkr::desk::ProRataAllocator allocator(
      tkr::desk::ProRataAllocatorConfig{true});
  tkr::desk::ProRataAllocatorResult result = allocator.AllocateWithWeights(
      static_cast<std::uint32_t>(total_qty), weights, records);

  if (result.status != tkr::Status::kOk) {
    std::cerr << "allocation failed: " << tkr::StatusToString(result.status)
              << "\n";
    return 1;
  }

  std::cout << "Pro-rata allocation of " << total_qty << " units:\n";
  for (const tkr::desk::ProRataSlice& slice : result.slices) {
    std::cout << "  account " << slice.account_id << ": " << slice.qty_milli
              << "\n";
  }
  std::cout << "Total allocated: " << result.total_allocated_milli << "\n";
  return 0;
}

int CmdPipeline(const char* path) {
  FILE* fp = std::fopen(path, "rb");
  if (fp == nullptr) {
    std::cerr << "cannot open " << path << "\n";
    return 1;
  }

  std::fseek(fp, 0, SEEK_END);
  const long size = std::ftell(fp);
  std::fseek(fp, 0, SEEK_SET);

  if (size <= 0) {
    std::cerr << "empty file\n";
    std::fclose(fp);
    return 1;
  }

  std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
  const std::size_t read =
      std::fread(data.data(), 1, data.size(), fp);
  std::fclose(fp);

  const tkr::Status st =
      tkr::engine::RunAllocationPipeline(data.data(), read);
  std::cout << "pipeline status: " << tkr::StatusToString(st) << "\n";
  return st == tkr::Status::kOk ? 0 : 1;
}

int CmdStats() {
  const tkr::engine::PipelineStats& stats =
      tkr::engine::LastPipelineStats();
  std::cout << "batches_processed=" << stats.batches_processed << "\n"
            << "envelopes_sealed=" << stats.envelopes_sealed << "\n"
            << "sessions_merged=" << stats.sessions_merged << "\n"
            << "desk_calls=" << stats.desk_calls << "\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    PrintUsage(argv[0]);
    return 1;
  }

  const std::string cmd = argv[1];
  if (cmd == "allocate") {
    return CmdAllocate(argc, argv);
  }
  if (cmd == "pipeline") {
    if (argc < 3) {
      std::cerr << "pipeline requires file path\n";
      return 1;
    }
    return CmdPipeline(argv[2]);
  }
  if (cmd == "stats") {
    return CmdStats();
  }

  PrintUsage(argv[0]);
  return 1;
}
