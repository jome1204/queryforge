#include "internal.h"

#include <iomanip>

namespace queryforge {
namespace {
uint64_t hash_bytes(const uint8_t *data, size_t size) {
  uint64_t hash = 1469598103934665603ull;
  for (size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= 1099511628211ull;
  }
  return hash;
}
std::string escape_json(std::string_view input) {
  std::ostringstream output;
  for (unsigned char c : input) {
    if (c == '"')
      output << "\\\"";
    else if (c == '\\')
      output << "\\\\";
    else if (c == '\n')
      output << "\\n";
    else if (c == '\r')
      output << "\\r";
    else if (c == '\t')
      output << "\\t";
    else if (c < 32)
      output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
             << static_cast<unsigned>(c) << std::dec;
    else
      output << static_cast<char>(c);
  }
  return output.str();
}
std::string plan_name(PlanKind kind) {
  switch (kind) {
  case PlanKind::constant:
    return "Constant";
  case PlanKind::table_scan:
    return "TableScan";
  case PlanKind::index_scan:
    return "IndexScan";
  case PlanKind::filter:
    return "Filter";
  case PlanKind::projection:
    return "Projection";
  case PlanKind::nested_loop_join:
    return "NestedLoopJoin";
  case PlanKind::aggregate:
    return "Aggregate";
  case PlanKind::sort:
    return "Sort";
  case PlanKind::limit:
    return "Limit";
  case PlanKind::insert:
    return "Insert";
  case PlanKind::update:
    return "Update";
  case PlanKind::remove:
    return "Delete";
  }
  return "Unknown";
}
void plan_text(const PlanNode &plan, std::ostringstream &output,
               uint32_t depth) {
  output << std::string(depth * 2, ' ') << plan_name(plan.kind);
  if (!plan.table.empty())
    output << " table=" << plan.table;
  if (!plan.index.empty())
    output << " index=" << plan.index;
  output << " rows=" << plan.estimated_rows << " cost=" << std::fixed
         << std::setprecision(2) << plan.estimated_cost << '\n';
  for (const auto &child : plan.children)
    plan_text(*child, output, depth + 1);
}
void plan_json(const PlanNode &plan, std::ostringstream &output) {
  output << "{\"kind\":\"" << plan_name(plan.kind) << "\",\"table\":\""
         << escape_json(plan.table) << "\",\"index\":\""
         << escape_json(plan.index)
         << "\",\"estimated_rows\":" << plan.estimated_rows
         << ",\"estimated_cost\":" << plan.estimated_cost << ",\"children\":[";
  for (size_t i = 0; i < plan.children.size(); ++i) {
    if (i)
      output << ',';
    plan_json(*plan.children[i], output);
  }
  output << "]}";
}
std::string issue_severity(IntegrityIssue::Severity severity) {
  switch (severity) {
  case IntegrityIssue::Severity::information:
    return "information";
  case IntegrityIssue::Severity::warning:
    return "warning";
  case IntegrityIssue::Severity::error:
    return "error";
  }
  return "unknown";
}
} // namespace

StatisticsCollector::StatisticsCollector(Limits limits) : limits_(limits) {}
std::optional<TableStatistics>
StatisticsCollector::collect(const Schema &schema,
                             const std::vector<RowRecord> &rows,
                             Error &error) const {
  error.clear();
  if (schema.columns.empty() || schema.columns.size() > limits_.max_columns ||
      rows.size() > limits_.max_rows) {
    internal::fail(error, ErrorCode::resource_limit, 0,
                   "statistics input exceeds limits");
    return std::nullopt;
  }
  TableStatistics result;
  result.table = schema.name;
  result.null_counts.assign(schema.columns.size(), 0);
  result.distinct_estimates.assign(schema.columns.size(), 0);
  std::vector<std::set<std::string>> distinct(schema.columns.size());
  for (const RowRecord &row : rows) {
    if (row.deleted) {
      ++result.deleted_rows;
      continue;
    }
    ++result.live_rows;
    if (row.values.size() != schema.columns.size()) {
      internal::fail(error, ErrorCode::constraint_error, row.row_id,
                     "row width differs from statistics schema");
      return std::nullopt;
    }
    result.estimated_bytes += sizeof(RowRecord);
    for (size_t column = 0; column < row.values.size(); ++column) {
      result.estimated_bytes += row.values[column].memory_usage();
      if (row.values[column].is_null())
        ++result.null_counts[column];
      if (distinct[column].size() < 100000)
        distinct[column].insert(row.values[column].display());
    }
  }
  for (size_t column = 0; column < distinct.size(); ++column)
    result.distinct_estimates[column] = distinct[column].size();
  return result;
}

std::string StatisticsCollector::json(const TableStatistics &statistics) {
  std::ostringstream output;
  output << "{\"table\":\"" << escape_json(statistics.table)
         << "\",\"live_rows\":" << statistics.live_rows
         << ",\"deleted_rows\":" << statistics.deleted_rows
         << ",\"estimated_bytes\":" << statistics.estimated_bytes
         << ",\"null_counts\":[";
  for (size_t i = 0; i < statistics.null_counts.size(); ++i) {
    if (i)
      output << ',';
    output << statistics.null_counts[i];
  }
  output << "],\"distinct_estimates\":[";
  for (size_t i = 0; i < statistics.distinct_estimates.size(); ++i) {
    if (i)
      output << ',';
    output << statistics.distinct_estimates[i];
  }
  output << "]}";
  return output.str();
}

IntegrityChecker::IntegrityChecker(Limits limits) : limits_(limits) {}
std::vector<IntegrityIssue>
IntegrityChecker::check_database(const Database &database) const {
  std::vector<IntegrityIssue> output;
  for (const std::string &issue : database.verify())
    output.push_back({IntegrityIssue::Severity::error, "database", 0, issue});
  for (const auto &table : database.catalog().tables()) {
    auto rows = database.rows().find(table.first);
    if (rows == database.rows().end())
      continue;
    Error error;
    if (!StatisticsCollector(limits_).collect(table.second, rows->second,
                                              error))
      output.push_back({IntegrityIssue::Severity::error, table.first,
                        error.offset, error.message});
  }
  return output;
}
std::vector<IntegrityIssue>
IntegrityChecker::check_pager(const Pager &pager) const {
  std::vector<IntegrityIssue> output;
  if (pager.page_count() == 0)
    output.push_back({IntegrityIssue::Severity::error, "pager", 0,
                      "pager has no catalog page"});
  for (uint64_t page_id = 0; page_id < pager.page_count(); ++page_id) {
    const Page *page = pager.page(static_cast<uint32_t>(page_id));
    Error error;
    if (!page || !page->verify(error))
      output.push_back({IntegrityIssue::Severity::error, "page", page_id,
                        page ? error.message : "page is absent"});
    else {
      auto header = page->header(error);
      if (header && header->page_id != page_id)
        output.push_back({IntegrityIssue::Severity::error, "page", page_id,
                          "page identifier differs from slot"});
      if (header && header->right_page >= pager.page_count() &&
          header->right_page != 0)
        output.push_back({IntegrityIssue::Severity::error, "page", page_id,
                          "right-page link exceeds pager"});
    }
  }
  return output;
}
std::vector<IntegrityIssue>
IntegrityChecker::check_wal(const std::vector<WalRecord> &records) const {
  std::vector<IntegrityIssue> output;
  uint64_t previous = 0;
  std::set<uint64_t> begun;
  std::set<uint64_t> finished;
  for (const WalRecord &record : records) {
    if (record.lsn <= previous)
      output.push_back({IntegrityIssue::Severity::error, "wal", record.lsn,
                        "LSN is not strictly increasing"});
    previous = record.lsn;
    if (record.payload.size() > limits_.max_record_bytes)
      output.push_back({IntegrityIssue::Severity::error, "wal", record.lsn,
                        "record payload exceeds limit"});
    if (record.type == WalRecordType::begin)
      begun.insert(record.transaction_id);
    if (record.type == WalRecordType::commit ||
        record.type == WalRecordType::rollback)
      finished.insert(record.transaction_id);
  }
  for (uint64_t transaction : begun)
    if (!finished.count(transaction))
      output.push_back({IntegrityIssue::Severity::warning, "wal", transaction,
                        "transaction is incomplete"});
  return output;
}

std::string PlanFormatter::text(const PlanNode &plan) {
  std::ostringstream output;
  plan_text(plan, output, 0);
  return output.str();
}
std::string PlanFormatter::json(const PlanNode &plan) {
  std::ostringstream output;
  plan_json(plan, output);
  return output.str();
}
uint64_t PlanFormatter::node_count(const PlanNode &plan) {
  uint64_t count = 1;
  for (const auto &child : plan.children)
    count += node_count(*child);
  return count;
}
uint64_t PlanFormatter::maximum_depth(const PlanNode &plan) {
  uint64_t depth = 1;
  for (const auto &child : plan.children)
    depth = std::max(depth, uint64_t{1} + maximum_depth(*child));
  return depth;
}

ReportBuilder::ReportBuilder(Limits limits) : limits_(limits) {}
DatabaseReport ReportBuilder::build(const Database &database,
                                    Error &error) const {
  error.clear();
  DatabaseReport report;
  report.table_count = database.catalog().tables().size();
  report.index_count = database.catalog().indexes().size();
  report.invariant_hash = database.invariant_hash();
  for (const auto &table : database.catalog().tables()) {
    auto rows = database.rows().find(table.first);
    if (rows == database.rows().end())
      continue;
    auto statistics =
        StatisticsCollector(limits_).collect(table.second, rows->second, error);
    if (!statistics)
      return {};
    report.live_rows += statistics->live_rows;
    report.deleted_rows += statistics->deleted_rows;
    report.logical_bytes += statistics->estimated_bytes;
    report.tables.push_back(std::move(*statistics));
  }
  report.issues = IntegrityChecker(limits_).check_database(database);
  return report;
}
std::string ReportBuilder::json(const DatabaseReport &report) {
  std::ostringstream output;
  output << "{\"table_count\":" << report.table_count
         << ",\"index_count\":" << report.index_count
         << ",\"live_rows\":" << report.live_rows
         << ",\"deleted_rows\":" << report.deleted_rows
         << ",\"logical_bytes\":" << report.logical_bytes
         << ",\"invariant_hash\":" << report.invariant_hash << ",\"tables\":[";
  for (size_t i = 0; i < report.tables.size(); ++i) {
    if (i)
      output << ',';
    output << StatisticsCollector::json(report.tables[i]);
  }
  output << "],\"issues\":[";
  for (size_t i = 0; i < report.issues.size(); ++i) {
    if (i)
      output << ',';
    output << "{\"severity\":\"" << issue_severity(report.issues[i].severity)
           << "\",\"object\":\"" << escape_json(report.issues[i].object)
           << "\",\"offset\":" << report.issues[i].offset << ",\"message\":\""
           << escape_json(report.issues[i].message) << "\"}";
  }
  output << "]}";
  return output.str();
}
std::string ReportBuilder::text(const DatabaseReport &report) {
  std::ostringstream output;
  output << "Tables: " << report.table_count
         << "\nIndexes: " << report.index_count
         << "\nLive rows: " << report.live_rows
         << "\nDeleted rows: " << report.deleted_rows
         << "\nLogical bytes: " << report.logical_bytes
         << "\nInvariant hash: " << report.invariant_hash << '\n';
  for (const auto &table : report.tables)
    output << "  " << table.table << ": " << table.live_rows << " live, "
           << table.deleted_rows << " deleted\n";
  for (const auto &issue : report.issues)
    output << "  [" << issue_severity(issue.severity) << "] " << issue.object
           << ": " << issue.message << '\n';
  return output.str();
}

} // namespace queryforge
