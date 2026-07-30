#include "queryforge/database.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  queryforge::Limits limits;
  limits.max_sql_bytes = 256 * 1024;
  limits.max_statements = 256;
  limits.max_parser_depth = 64;
  limits.max_expression_nodes = 8192;
  if (size > limits.max_sql_bytes)
    return 0;
  queryforge::Parser parser(limits);
  auto parsed =
      parser.parse(std::string_view(reinterpret_cast<const char *>(data), size));
  if (!parsed)
    return 0;
  queryforge::Catalog catalog(limits);
  queryforge::QueryPlanner planner(limits);
  queryforge::SqlFormatter formatter(limits);
  for (const auto &statement : *parsed.statements) {
    queryforge::Error error;
    (void)planner.plan(statement, catalog, error);
    queryforge::Error format_error;
    (void)formatter.format(
        std::string_view(reinterpret_cast<const char *>(data), size),
        format_error);
  }
  return 0;
}
