#include "internal.h"

namespace queryforge {

Catalog::Catalog(Limits limits) : limits_(limits) {}

bool Catalog::create_table(Schema schema, Error &error) {
  error.clear();
  std::string key = internal::normalize(schema.name);
  if (key.empty() || key.size() > limits_.max_identifier_bytes ||
      schema.columns.empty() || schema.columns.size() > limits_.max_columns)
    return internal::fail(error, ErrorCode::semantic_error, 0,
                          "table schema dimensions are invalid");
  if (tables_.size() >= limits_.max_tables)
    return internal::fail(error, ErrorCode::resource_limit, 0,
                          "table count exceeds limit");
  if (tables_.find(key) != tables_.end())
    return internal::fail(error, ErrorCode::duplicate, 0,
                          "table already exists");
  std::set<std::string> names;
  size_t primary_keys = 0;
  for (const Column &column : schema.columns) {
    std::string column_key = internal::normalize(column.name);
    if (column_key.empty() ||
        column_key.size() > limits_.max_identifier_bytes ||
        !names.insert(column_key).second || column.type == DataType::null_type)
      return internal::fail(error, ErrorCode::semantic_error, 0,
                            "table contains an invalid or duplicate column");
    if (column.primary_key)
      ++primary_keys;
    if (column.default_value && !column.default_value->is_null() &&
        column.default_value->type() != column.type &&
        !(column.type == DataType::real &&
          column.default_value->type() == DataType::integer))
      return internal::fail(error, ErrorCode::type_error, 0,
                            "column default has incompatible type");
  }
  if (primary_keys > 1)
    return internal::fail(error, ErrorCode::semantic_error, 0,
                          "composite primary keys are not supported");
  tables_.emplace(std::move(key), std::move(schema));
  return true;
}

bool Catalog::drop_table(std::string_view name, Error &error) {
  error.clear();
  std::string key = internal::normalize(name);
  auto found = tables_.find(key);
  if (found == tables_.end())
    return internal::fail(error, ErrorCode::not_found, 0,
                          "table does not exist");
  tables_.erase(found);
  for (auto iterator = indexes_.begin(); iterator != indexes_.end();) {
    if (internal::normalize(iterator->second.table) == key)
      iterator = indexes_.erase(iterator);
    else
      ++iterator;
  }
  return true;
}

bool Catalog::create_index(IndexDefinition index, Error &error) {
  error.clear();
  std::string key = internal::normalize(index.name);
  const Schema *schema = table(index.table);
  if (!schema)
    return internal::fail(error, ErrorCode::not_found, 0,
                          "indexed table does not exist");
  if (key.empty() || key.size() > limits_.max_identifier_bytes ||
      index.columns.empty() || index.columns.size() > limits_.max_columns ||
      indexes_.size() >= limits_.max_indexes)
    return internal::fail(error, ErrorCode::resource_limit, 0,
                          "index definition exceeds limits");
  if (indexes_.find(key) != indexes_.end())
    return internal::fail(error, ErrorCode::duplicate, 0,
                          "index already exists");
  std::set<size_t> columns;
  for (size_t column : index.columns)
    if (column >= schema->columns.size() || !columns.insert(column).second)
      return internal::fail(error, ErrorCode::semantic_error, 0,
                            "index column is invalid or duplicated");
  indexes_.emplace(std::move(key), std::move(index));
  return true;
}

bool Catalog::drop_index(std::string_view name, Error &error) {
  error.clear();
  auto found = indexes_.find(internal::normalize(name));
  if (found == indexes_.end())
    return internal::fail(error, ErrorCode::not_found, 0,
                          "index does not exist");
  indexes_.erase(found);
  return true;
}

const Schema *Catalog::table(std::string_view name) const {
  auto found = tables_.find(internal::normalize(name));
  return found == tables_.end() ? nullptr : &found->second;
}
Schema *Catalog::table(std::string_view name) {
  auto found = tables_.find(internal::normalize(name));
  return found == tables_.end() ? nullptr : &found->second;
}
const IndexDefinition *Catalog::index(std::string_view name) const {
  auto found = indexes_.find(internal::normalize(name));
  return found == indexes_.end() ? nullptr : &found->second;
}

} // namespace queryforge
