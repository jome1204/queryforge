#include "internal.h"

namespace queryforge {

QueryPlanner::QueryPlanner(Limits limits) : limits_(limits) {}

std::optional<PlanNode> QueryPlanner::plan(const Statement &statement,
                                           const Catalog &catalog,
                                           Error &error) const {
  error.clear();
  if (!SemanticAnalyzer(limits_).analyze(statement, catalog, error))
    return std::nullopt;
  if (auto select = std::get_if<SelectStatement>(&statement.data)) {
    PlanNode root;
    if (select->table.empty()) {
      root.kind = PlanKind::constant;
      root.estimated_rows = 1;
      root.estimated_cost = 1;
    } else {
      root.kind = PlanKind::table_scan;
      root.table = select->table;
      root.estimated_rows = 1000;
      root.estimated_cost = 1000;
      for (const auto &catalog_index : catalog.indexes()) {
        if (internal::normalize(catalog_index.second.table) ==
            internal::normalize(select->table)) {
          root.kind = PlanKind::index_scan;
          root.index = catalog_index.second.name;
          root.estimated_rows = 100;
          root.estimated_cost = 100;
          break;
        }
      }
    }
    for (const JoinClause &join : select->joins) {
      auto joined = std::make_unique<PlanNode>();
      joined->kind = PlanKind::table_scan;
      joined->table = join.table;
      joined->estimated_rows = 1000;
      joined->estimated_cost = 1000;
      PlanNode combined;
      combined.kind = PlanKind::nested_loop_join;
      combined.expression = std::make_unique<Expression>(*join.condition);
      combined.children.push_back(std::make_unique<PlanNode>(std::move(root)));
      combined.children.push_back(std::move(joined));
      combined.estimated_rows = combined.children[0]->estimated_rows *
                                combined.children[1]->estimated_rows / 10;
      combined.estimated_cost = combined.children[0]->estimated_cost +
                                combined.children[0]->estimated_rows *
                                    combined.children[1]->estimated_cost;
      root = std::move(combined);
    }
    if (select->where) {
      PlanNode filter;
      filter.kind = PlanKind::filter;
      filter.expression = std::make_unique<Expression>(*select->where);
      filter.children.push_back(std::make_unique<PlanNode>(std::move(root)));
      filter.estimated_rows = filter.children[0]->estimated_rows / 3;
      filter.estimated_cost = filter.children[0]->estimated_cost +
                              filter.children[0]->estimated_rows;
      root = std::move(filter);
    }
    if (!select->group_by.empty()) {
      PlanNode aggregate;
      aggregate.kind = PlanKind::aggregate;
      for (const auto &expression : select->group_by)
        aggregate.projections.push_back(
            std::make_unique<Expression>(*expression));
      aggregate.children.push_back(std::make_unique<PlanNode>(std::move(root)));
      aggregate.estimated_rows =
          std::max<uint64_t>(1, aggregate.children[0]->estimated_rows / 10);
      aggregate.estimated_cost = aggregate.children[0]->estimated_cost +
                                 aggregate.children[0]->estimated_rows;
      root = std::move(aggregate);
    }
    PlanNode projection;
    projection.kind = PlanKind::projection;
    for (const SelectItem &item : select->items)
      if (!item.wildcard)
        projection.projections.push_back(
            std::make_unique<Expression>(*item.expression));
    projection.children.push_back(std::make_unique<PlanNode>(std::move(root)));
    projection.estimated_rows = projection.children[0]->estimated_rows;
    projection.estimated_cost =
        projection.children[0]->estimated_cost + projection.estimated_rows;
    root = std::move(projection);
    if (!select->order_by.empty()) {
      PlanNode sort;
      sort.kind = PlanKind::sort;
      for (const OrderItem &item : select->order_by)
        sort.projections.push_back(
            std::make_unique<Expression>(*item.expression));
      sort.children.push_back(std::make_unique<PlanNode>(std::move(root)));
      sort.estimated_rows = sort.children[0]->estimated_rows;
      sort.estimated_cost =
          sort.children[0]->estimated_cost +
          sort.estimated_rows *
              std::log2(static_cast<double>(sort.estimated_rows + 1));
      root = std::move(sort);
    }
    if (select->limit) {
      PlanNode limit;
      limit.kind = PlanKind::limit;
      limit.children.push_back(std::make_unique<PlanNode>(std::move(root)));
      limit.estimated_rows =
          std::min(*select->limit, limit.children[0]->estimated_rows);
      limit.estimated_cost = limit.children[0]->estimated_cost;
      root = std::move(limit);
    }
    return root;
  }
  PlanNode output;
  if (std::holds_alternative<InsertStatement>(statement.data))
    output.kind = PlanKind::insert;
  else if (std::holds_alternative<UpdateStatement>(statement.data))
    output.kind = PlanKind::update;
  else if (std::holds_alternative<DeleteStatement>(statement.data))
    output.kind = PlanKind::remove;
  else
    output.kind = PlanKind::constant;
  output.estimated_rows = 1;
  output.estimated_cost = 1;
  return output;
}

} // namespace queryforge
