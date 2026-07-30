#include "internal.h"

namespace queryforge {
namespace {

ExecutionResult failure(ErrorCode code, uint64_t offset, std::string message) {
  ExecutionResult result;
  internal::fail(result.error, code, offset, std::move(message));
  return result;
}
ExecutionResult success(std::string message = {}) {
  QueryResult result;
  result.message = std::move(message);
  return {std::move(result), {}};
}

std::vector<Binding> bindings_for(const Schema &schema,
                                  std::string_view qualifier,
                                  size_t offset = 0) {
  std::vector<Binding> output;
  for (size_t i = 0; i < schema.columns.size(); ++i)
    output.push_back({std::string(qualifier), schema.columns[i].name,
                      schema.columns[i].type, offset + i});
  return output;
}

bool compatible(const Value &value, const Column &column) {
  return value.is_null() ? column.nullable
                         : (value.type() == column.type ||
                            (column.type == DataType::real &&
                             value.type() == DataType::integer));
}

std::string select_name(const SelectItem &item, size_t index) {
  if (!item.alias.empty())
    return item.alias;
  if (item.expression && item.expression->kind == Expression::Kind::column)
    return item.expression->name;
  if (item.expression && item.expression->kind == Expression::Kind::function)
    return internal::normalize(item.expression->name);
  return "column" + std::to_string(index + 1);
}

bool aggregate_function(const Expression &expression) {
  if (expression.kind != Expression::Kind::function)
    return false;
  std::string name = internal::normalize(expression.name);
  return name == "COUNT" || name == "SUM" || name == "AVG" || name == "MIN" ||
         name == "MAX";
}

std::optional<Value> aggregate(const Expression &expression,
                               const std::vector<Row> &rows,
                               const std::vector<Binding> &bindings,
                               const Limits &limits, Error &error) {
  std::string name = internal::normalize(expression.name);
  if (name == "COUNT" && expression.arguments.size() == 1 &&
      expression.arguments[0]->kind == Expression::Kind::column &&
      expression.arguments[0]->name == "*")
    return Value(static_cast<int64_t>(rows.size()));
  if (expression.arguments.size() != 1) {
    internal::fail(error, ErrorCode::semantic_error, expression.offset,
                   "aggregate requires one argument");
    return std::nullopt;
  }
  ExpressionEvaluator evaluator(limits);
  uint64_t count = 0;
  long double sum = 0.0;
  std::optional<Value> extreme;
  for (const Row &row : rows) {
    auto value =
        evaluator.evaluate(*expression.arguments[0], row, bindings, error);
    if (!value)
      return std::nullopt;
    if (value->is_null())
      continue;
    ++count;
    if (name == "COUNT")
      continue;
    if (name == "SUM" || name == "AVG") {
      auto number = value->as_real();
      if (!number) {
        internal::fail(error, ErrorCode::type_error, expression.offset,
                       "SUM and AVG require numeric values");
        return std::nullopt;
      }
      sum += *number;
    } else if (!extreme) {
      extreme = *value;
    } else {
      CompareResult comparison = compare_values(*value, *extreme);
      if ((name == "MIN" && comparison == CompareResult::less) ||
          (name == "MAX" && comparison == CompareResult::greater))
        extreme = *value;
    }
  }
  if (name == "COUNT")
    return Value(static_cast<int64_t>(count));
  if (name == "SUM")
    return count ? Value(static_cast<double>(sum)) : Value();
  if (name == "AVG")
    return count ? Value(static_cast<double>(sum / count)) : Value();
  return extreme.value_or(Value());
}

void append_string(std::vector<uint8_t> &output, std::string_view value) {
  internal::append32(output, static_cast<uint32_t>(value.size()));
  output.insert(output.end(), value.begin(), value.end());
}
bool read_string(const uint8_t *data, size_t size, size_t &position,
                 uint32_t maximum, std::string &output, Error &error) {
  if (size - position < 4) {
    internal::fail(error, ErrorCode::truncated, position,
                   "database string length is truncated");
    return false;
  }
  uint32_t length = internal::le32(data + position);
  position += 4;
  if (length > maximum || length > size - position) {
    internal::fail(error, ErrorCode::resource_limit, position,
                   "database string exceeds bounds");
    return false;
  }
  output.assign(reinterpret_cast<const char *>(data + position), length);
  position += length;
  return true;
}
} // namespace

Database::Database(Limits limits)
    : limits_(limits), catalog_(limits), pager_(limits), transaction_(limits) {
  Error ignored;
  pager_.create(ignored);
}

ExecutionResult Database::execute(std::string_view sql) {
  ParseResult parsed = Parser(limits_).parse(sql);
  if (!parsed)
    return {std::nullopt, std::move(parsed.error)};
  QueryResult combined;
  for (const Statement &statement : *parsed.statements) {
    ExecutionResult current = execute(statement);
    if (!current)
      return current;
    combined.affected_rows += current.result->affected_rows;
    combined.columns = std::move(current.result->columns);
    combined.rows = std::move(current.result->rows);
    combined.message = std::move(current.result->message);
  }
  return {std::move(combined), {}};
}

ExecutionResult Database::execute(const Statement &statement) {
  Error semantic_error;
  if (!std::holds_alternative<CreateTableStatement>(statement.data) &&
      !std::holds_alternative<TransactionCommand>(statement.data) &&
      !std::holds_alternative<DropStatement>(statement.data) &&
      !SemanticAnalyzer(limits_).analyze(statement, catalog_, semantic_error))
    return {std::nullopt, std::move(semantic_error)};
  if (auto value = std::get_if<SelectStatement>(&statement.data))
    return execute_select(*value);
  if (auto value = std::get_if<InsertStatement>(&statement.data))
    return execute_insert(*value);
  if (auto value = std::get_if<UpdateStatement>(&statement.data))
    return execute_update(*value);
  if (auto value = std::get_if<DeleteStatement>(&statement.data))
    return execute_delete(*value);
  if (auto value = std::get_if<CreateTableStatement>(&statement.data))
    return execute_create_table(*value);
  if (auto value = std::get_if<CreateIndexStatement>(&statement.data))
    return execute_create_index(*value);
  if (auto value = std::get_if<DropStatement>(&statement.data))
    return execute_drop(*value);
  TransactionCommand command = std::get<TransactionCommand>(statement.data);
  Error error;
  if (command == TransactionCommand::begin) {
    if (!transaction_.begin(error))
      return {std::nullopt, std::move(error)};
    return success("transaction started");
  }
  if (command == TransactionCommand::commit) {
    if (!transaction_.commit(error))
      return {std::nullopt, std::move(error)};
    return success("transaction committed");
  }
  if (command == TransactionCommand::rollback) {
    if (!transaction_.rollback(rows_, catalog_, error))
      return {std::nullopt, std::move(error)};
    indexes_.clear();
    return success("transaction rolled back");
  }
  if (!checkpoint(error))
    return {std::nullopt, std::move(error)};
  return success("checkpoint complete");
}

ExecutionResult
Database::execute_create_table(const CreateTableStatement &create) {
  Schema schema;
  schema.name = create.table;
  schema.identifier = catalog_.tables().size() + 1;
  schema.generation = 1;
  for (const ColumnDefinition &definition : create.columns)
    schema.columns.push_back({definition.name, definition.type,
                              definition.nullable, definition.primary_key,
                              definition.unique, definition.default_value});
  Error error;
  if (!catalog_.create_table(schema, error))
    return {std::nullopt, std::move(error)};
  rows_[internal::normalize(create.table)] = {};
  if (transaction_.state() == TransactionState::active &&
      !transaction_.record({UndoEntry::Kind::create_table, create.table},
                           error)) {
    catalog_.drop_table(create.table, error);
    rows_.erase(internal::normalize(create.table));
    return {std::nullopt, std::move(error)};
  }
  for (size_t i = 0; i < schema.columns.size(); ++i) {
    if (!schema.columns[i].primary_key && !schema.columns[i].unique)
      continue;
    IndexDefinition index;
    index.name = create.table + "_" + schema.columns[i].name + "_auto";
    index.table = create.table;
    index.columns = {i};
    index.unique = true;
    catalog_.create_index(index, error);
    BTree tree(limits_);
    uint32_t page = pager_.allocate(PageType::btree_leaf, error);
    if (page && tree.initialize(page, true, error))
      indexes_.emplace(internal::normalize(index.name), std::move(tree));
  }
  return success("table created");
}

ExecutionResult
Database::execute_create_index(const CreateIndexStatement &create) {
  const Schema *schema = catalog_.table(create.table);
  if (!schema)
    return failure(ErrorCode::not_found, 0, "indexed table does not exist");
  IndexDefinition definition;
  definition.name = create.index;
  definition.table = create.table;
  definition.unique = create.unique;
  for (const std::string &column : create.columns) {
    auto index = schema->column_index(column);
    if (!index)
      return failure(ErrorCode::not_found, 0, "indexed column does not exist");
    definition.columns.push_back(*index);
  }
  Error error;
  if (!catalog_.create_index(definition, error))
    return {std::nullopt, std::move(error)};
  BTree tree(limits_);
  uint32_t page = pager_.allocate(PageType::btree_leaf, error);
  if (!page || !tree.initialize(page, create.unique, error))
    return {std::nullopt, std::move(error)};
  RecordCodec codec(limits_);
  for (const RowRecord &row : rows_[internal::normalize(create.table)]) {
    if (row.deleted)
      continue;
    auto key = codec.encode_key(row.values, definition.columns, error);
    if (error || !tree.insert(std::move(key), row.row_id, error)) {
      catalog_.drop_index(create.index, error);
      return {std::nullopt, std::move(error)};
    }
  }
  indexes_[internal::normalize(create.index)] = std::move(tree);
  return success("index created");
}

ExecutionResult Database::execute_insert(const InsertStatement &insert) {
  const Schema *schema = catalog_.table(insert.table);
  if (!schema)
    return failure(ErrorCode::not_found, 0, "table does not exist");
  std::vector<size_t> destinations;
  if (insert.columns.empty()) {
    for (size_t i = 0; i < schema->columns.size(); ++i)
      destinations.push_back(i);
  } else {
    for (const std::string &name : insert.columns) {
      auto column = schema->column_index(name);
      if (!column)
        return failure(ErrorCode::not_found, 0, "insert column does not exist");
      if (std::find(destinations.begin(), destinations.end(), *column) !=
          destinations.end())
        return failure(ErrorCode::duplicate, 0, "insert column is duplicated");
      destinations.push_back(*column);
    }
  }
  auto &table_rows = rows_[internal::normalize(insert.table)];
  ExpressionEvaluator evaluator(limits_);
  Error error;
  uint64_t inserted = 0;
  for (const auto &source : insert.rows) {
    if (source.size() != destinations.size())
      return failure(ErrorCode::constraint_error, 0,
                     "insert value count differs from column count");
    Row values;
    values.reserve(schema->columns.size());
    for (const Column &column : schema->columns)
      values.push_back(column.default_value.value_or(Value()));
    for (size_t i = 0; i < source.size(); ++i) {
      auto value = evaluator.evaluate(*source[i], {}, {}, error);
      if (!value)
        return {std::nullopt, std::move(error)};
      if (!compatible(*value, schema->columns[destinations[i]]))
        return failure(ErrorCode::type_error, source[i]->offset,
                       "insert value has incompatible type or NULL");
      values[destinations[i]] = std::move(*value);
    }
    RowRecord record{next_row_id_++, 1, false, std::move(values)};
    for (const auto &index_entry : catalog_.indexes()) {
      if (internal::normalize(index_entry.second.table) !=
          internal::normalize(insert.table))
        continue;
      auto key = RecordCodec(limits_).encode_key(
          record.values, index_entry.second.columns, error);
      BTree &tree = indexes_[index_entry.first];
      if (!tree.insert(std::move(key), record.row_id, error))
        return {std::nullopt, std::move(error)};
    }
    table_rows.push_back(record);
    if (transaction_.state() == TransactionState::active &&
        !transaction_.record(
            {UndoEntry::Kind::insert_row, insert.table, record.row_id}, error))
      return {std::nullopt, std::move(error)};
    ++inserted;
  }
  QueryResult result;
  result.affected_rows = inserted;
  result.message = "rows inserted";
  return {std::move(result), {}};
}

ExecutionResult Database::execute_select(const SelectStatement &select) {
  std::vector<Row> working;
  std::vector<Binding> bindings;
  if (select.table.empty()) {
    working.push_back({});
  } else {
    const Schema *schema = catalog_.table(select.table);
    if (!schema)
      return failure(ErrorCode::not_found, 0, "table missing");
    bindings = bindings_for(*schema,
                            select.alias.empty() ? select.table : select.alias);
    for (const RowRecord &record : rows_[internal::normalize(select.table)])
      if (!record.deleted)
        working.push_back(record.values);
    for (const JoinClause &join : select.joins) {
      const Schema *joined_schema = catalog_.table(join.table);
      if (!joined_schema)
        return failure(ErrorCode::not_found, 0, "joined table missing");
      size_t prior_width = bindings.size();
      auto joined_bindings = bindings_for(
          *joined_schema, join.alias.empty() ? join.table : join.alias,
          prior_width);
      bindings.insert(bindings.end(), joined_bindings.begin(),
                      joined_bindings.end());
      std::vector<Row> joined_rows;
      Error error;
      ExpressionEvaluator evaluator(limits_);
      for (const Row &left : working) {
        bool matched = false;
        for (const RowRecord &right : rows_[internal::normalize(join.table)]) {
          if (right.deleted)
            continue;
          Row combined = left;
          combined.insert(combined.end(), right.values.begin(),
                          right.values.end());
          auto predicate =
              evaluator.predicate(*join.condition, combined, bindings, error);
          if (!predicate)
            return {std::nullopt, std::move(error)};
          if (*predicate) {
            matched = true;
            joined_rows.push_back(std::move(combined));
            if (joined_rows.size() > limits_.max_output_rows)
              return failure(ErrorCode::resource_limit, 0,
                             "join output exceeds row limit");
          }
        }
        if (!matched && join.kind == JoinClause::Kind::left) {
          Row combined = left;
          combined.resize(prior_width + joined_schema->columns.size());
          joined_rows.push_back(std::move(combined));
        }
      }
      working = std::move(joined_rows);
    }
  }
  Error error;
  ExpressionEvaluator evaluator(limits_);
  if (select.where) {
    working.erase(std::remove_if(working.begin(), working.end(),
                                 [&](const Row &row) {
                                   Error local;
                                   auto predicate = evaluator.predicate(
                                       *select.where, row, bindings, local);
                                   if (!predicate) {
                                     error = std::move(local);
                                     return false;
                                   }
                                   return !*predicate;
                                 }),
                  working.end());
    if (error)
      return {std::nullopt, std::move(error)};
  }
  bool has_aggregate = std::any_of(
      select.items.begin(), select.items.end(), [](const SelectItem &item) {
        return item.expression && aggregate_function(*item.expression);
      });
  QueryResult output;
  for (size_t i = 0; i < select.items.size(); ++i) {
    const SelectItem &item = select.items[i];
    if (item.wildcard) {
      for (const Binding &binding : bindings)
        output.columns.push_back(binding.name);
    } else {
      output.columns.push_back(select_name(item, i));
    }
  }
  if (has_aggregate) {
    Row projected;
    for (const SelectItem &item : select.items) {
      if (!item.expression || !aggregate_function(*item.expression))
        return failure(ErrorCode::semantic_error, 0,
                       "mixed aggregate projection requires GROUP BY");
      auto value =
          aggregate(*item.expression, working, bindings, limits_, error);
      if (!value)
        return {std::nullopt, std::move(error)};
      projected.push_back(std::move(*value));
    }
    output.rows.push_back(std::move(projected));
  } else {
    for (const Row &row : working) {
      Row projected;
      for (const SelectItem &item : select.items) {
        if (item.wildcard) {
          projected.insert(projected.end(), row.begin(), row.end());
        } else {
          auto value =
              evaluator.evaluate(*item.expression, row, bindings, error);
          if (!value)
            return {std::nullopt, std::move(error)};
          projected.push_back(std::move(*value));
        }
      }
      output.rows.push_back(std::move(projected));
      if (output.rows.size() > limits_.max_output_rows)
        return failure(ErrorCode::resource_limit, 0,
                       "query output exceeds row limit");
    }
  }
  if (!select.order_by.empty() && !has_aggregate) {
    // Ordering by projected ordinal is intentionally deterministic.
    std::stable_sort(output.rows.begin(), output.rows.end(),
                     [](const Row &left, const Row &right) {
                       if (left.empty() || right.empty())
                         return left.size() < right.size();
                       return compare_values(left[0], right[0]) ==
                              CompareResult::less;
                     });
    if (!select.order_by.front().ascending)
      std::reverse(output.rows.begin(), output.rows.end());
  }
  size_t offset = static_cast<size_t>(
      std::min<uint64_t>(select.offset, output.rows.size()));
  if (offset)
    output.rows.erase(output.rows.begin(), output.rows.begin() + offset);
  if (select.limit && output.rows.size() > *select.limit)
    output.rows.resize(static_cast<size_t>(*select.limit));
  output.message = "query complete";
  return {std::move(output), {}};
}

ExecutionResult Database::execute_update(const UpdateStatement &update) {
  const Schema *schema = catalog_.table(update.table);
  if (!schema)
    return failure(ErrorCode::not_found, 0, "table missing");
  auto bindings = bindings_for(*schema, update.table);
  std::vector<size_t> columns;
  for (const UpdateAssignment &assignment : update.assignments) {
    auto column = schema->column_index(assignment.column);
    if (!column)
      return failure(ErrorCode::not_found, 0, "update column missing");
    columns.push_back(*column);
  }
  Error error;
  ExpressionEvaluator evaluator(limits_);
  uint64_t affected = 0;
  for (RowRecord &record : rows_[internal::normalize(update.table)]) {
    if (record.deleted)
      continue;
    if (update.where) {
      auto match =
          evaluator.predicate(*update.where, record.values, bindings, error);
      if (!match)
        return {std::nullopt, std::move(error)};
      if (!*match)
        continue;
    }
    Row original = record.values;
    Row candidate = original;
    for (size_t i = 0; i < update.assignments.size(); ++i) {
      auto value = evaluator.evaluate(*update.assignments[i].expression,
                                      original, bindings, error);
      if (!value)
        return {std::nullopt, std::move(error)};
      if (!compatible(*value, schema->columns[columns[i]]))
        return failure(ErrorCode::type_error,
                       update.assignments[i].expression->offset,
                       "update value has incompatible type");
      candidate[columns[i]] = std::move(*value);
    }
    if (transaction_.state() == TransactionState::active &&
        !transaction_.record(
            {UndoEntry::Kind::update_row, update.table, record.row_id, record},
            error))
      return {std::nullopt, std::move(error)};
    record.values = std::move(candidate);
    ++record.generation;
    ++affected;
  }
  QueryResult output;
  output.affected_rows = affected;
  output.message = "rows updated";
  return {std::move(output), {}};
}

ExecutionResult Database::execute_delete(const DeleteStatement &remove) {
  const Schema *schema = catalog_.table(remove.table);
  if (!schema)
    return failure(ErrorCode::not_found, 0, "table missing");
  auto bindings = bindings_for(*schema, remove.table);
  Error error;
  ExpressionEvaluator evaluator(limits_);
  uint64_t affected = 0;
  for (RowRecord &record : rows_[internal::normalize(remove.table)]) {
    if (record.deleted)
      continue;
    if (remove.where) {
      auto match =
          evaluator.predicate(*remove.where, record.values, bindings, error);
      if (!match)
        return {std::nullopt, std::move(error)};
      if (!*match)
        continue;
    }
    if (transaction_.state() == TransactionState::active &&
        !transaction_.record(
            {UndoEntry::Kind::delete_row, remove.table, record.row_id, record},
            error))
      return {std::nullopt, std::move(error)};
    record.deleted = true;
    ++record.generation;
    ++affected;
  }
  QueryResult output;
  output.affected_rows = affected;
  output.message = "rows deleted";
  return {std::move(output), {}};
}

ExecutionResult Database::execute_drop(const DropStatement &drop) {
  Error error;
  if (drop.kind == DropStatement::Kind::index) {
    if (!catalog_.drop_index(drop.name, error))
      return {std::nullopt, std::move(error)};
    indexes_.erase(internal::normalize(drop.name));
    return success("index dropped");
  }
  const Schema *schema = catalog_.table(drop.name);
  if (!schema)
    return failure(ErrorCode::not_found, 0, "table does not exist");
  Schema copy = *schema;
  if (transaction_.state() == TransactionState::active &&
      !transaction_.record(
          {UndoEntry::Kind::drop_table, drop.name, 0, std::nullopt, copy},
          error))
    return {std::nullopt, std::move(error)};
  if (!catalog_.drop_table(drop.name, error))
    return {std::nullopt, std::move(error)};
  rows_.erase(internal::normalize(drop.name));
  return success("table dropped");
}

std::vector<uint8_t> Database::serialize(Error &error) const {
  error.clear();
  std::vector<uint8_t> output(32, 0);
  std::memcpy(output.data(), "QFENG1\0\0", 8);
  internal::patch32(output, 8, 1);
  internal::patch32(output, 12,
                    static_cast<uint32_t>(catalog_.tables().size()));
  uint64_t row_count = 0;
  for (const auto &table : rows_)
    row_count += table.second.size();
  for (unsigned shift = 0; shift < 64; shift += 8)
    output[16 + shift / 8] = static_cast<uint8_t>(row_count >> shift);
  internal::patch32(output, 24, limits_.page_size);
  for (const auto &table_entry : catalog_.tables()) {
    const Schema &schema = table_entry.second;
    append_string(output, schema.name);
    internal::append64(output, schema.identifier);
    internal::append64(output, schema.generation);
    internal::append32(output, static_cast<uint32_t>(schema.columns.size()));
    for (const Column &column : schema.columns) {
      append_string(output, column.name);
      output.push_back(static_cast<uint8_t>(column.type));
      output.push_back(column.nullable ? 1 : 0);
      output.push_back(column.primary_key ? 1 : 0);
      output.push_back(column.unique ? 1 : 0);
    }
    const auto &table_rows = rows_.at(table_entry.first);
    internal::append64(output, table_rows.size());
    for (const RowRecord &row : table_rows) {
      auto record = RecordCodec(limits_).encode(row, error);
      if (error)
        return {};
      internal::append32(output, static_cast<uint32_t>(record.size()));
      output.insert(output.end(), record.begin(), record.end());
    }
    if (output.size() > limits_.max_file_bytes) {
      internal::fail(error, ErrorCode::resource_limit, output.size(),
                     "database image exceeds file limit");
      return {};
    }
  }
  uint32_t checksum = crc32(output.data() + 32, output.size() - 32);
  internal::patch32(output, 28, checksum);
  return output;
}

bool Database::open(const uint8_t *data, size_t size, Error &error) {
  error.clear();
  if (size < 32 || size > limits_.max_file_bytes ||
      std::memcmp(data, "QFENG1\0\0", 8) != 0)
    return internal::fail(
        error, size < 32 ? ErrorCode::truncated : ErrorCode::invalid_signature,
        0, "database image header is invalid");
  if (internal::le32(data + 8) != 1 ||
      internal::le32(data + 24) != limits_.page_size)
    return internal::fail(error, ErrorCode::invalid_version, 8,
                          "database image version is incompatible");
  uint32_t tables = internal::le32(data + 12);
  uint64_t declared_rows = internal::le64(data + 16);
  if (tables > limits_.max_tables || declared_rows > limits_.max_rows)
    return internal::fail(error, ErrorCode::resource_limit, 12,
                          "database image counts exceed limits");
  if (crc32(data + 32, size - 32) != internal::le32(data + 28))
    return internal::fail(error, ErrorCode::checksum_mismatch, 28,
                          "database image checksum mismatch");
  Catalog loaded_catalog(limits_);
  std::map<std::string, std::vector<RowRecord>> loaded_rows;
  size_t position = 32;
  uint64_t actual_rows = 0;
  uint64_t maximum_row_id = 0;
  for (uint32_t table_index = 0; table_index < tables; ++table_index) {
    Schema schema;
    if (!read_string(data, size, position, limits_.max_identifier_bytes,
                     schema.name, error) ||
        size - position < 20)
      return false;
    schema.identifier = internal::le64(data + position);
    schema.generation = internal::le64(data + position + 8);
    uint32_t columns = internal::le32(data + position + 16);
    position += 20;
    if (columns == 0 || columns > limits_.max_columns)
      return internal::fail(error, ErrorCode::resource_limit, position - 4,
                            "database schema column count is invalid");
    for (uint32_t column_index = 0; column_index < columns; ++column_index) {
      Column column;
      if (!read_string(data, size, position, limits_.max_identifier_bytes,
                       column.name, error) ||
          size - position < 4)
        return false;
      column.type = static_cast<DataType>(data[position]);
      column.nullable = data[position + 1] != 0;
      column.primary_key = data[position + 2] != 0;
      column.unique = data[position + 3] != 0;
      position += 4;
      schema.columns.push_back(std::move(column));
    }
    if (!loaded_catalog.create_table(schema, error))
      return false;
    if (size - position < 8)
      return internal::fail(error, ErrorCode::truncated, position,
                            "table row count is truncated");
    uint64_t row_count = internal::le64(data + position);
    position += 8;
    if (row_count > limits_.max_rows - actual_rows)
      return internal::fail(error, ErrorCode::resource_limit, position - 8,
                            "table row count exceeds limit");
    auto &destination = loaded_rows[internal::normalize(schema.name)];
    destination.reserve(static_cast<size_t>(row_count));
    for (uint64_t row_index = 0; row_index < row_count; ++row_index) {
      if (size - position < 4)
        return internal::fail(error, ErrorCode::truncated, position,
                              "record length is truncated");
      uint32_t length = internal::le32(data + position);
      position += 4;
      if (length > size - position)
        return internal::fail(error, ErrorCode::invalid_offset, position,
                              "record exceeds database image");
      auto record = RecordCodec(limits_).decode(data + position, length, error);
      if (!record) {
        error.offset += position;
        return false;
      }
      if (record->values.size() != schema.columns.size())
        return internal::fail(error, ErrorCode::constraint_error, position,
                              "record width differs from schema");
      maximum_row_id = std::max(maximum_row_id, record->row_id);
      destination.push_back(std::move(*record));
      position += length;
      ++actual_rows;
    }
  }
  if (position != size || actual_rows != declared_rows)
    return internal::fail(error, ErrorCode::invalid_offset, position,
                          "database image counts or trailing bytes mismatch");
  catalog_ = std::move(loaded_catalog);
  rows_ = std::move(loaded_rows);
  indexes_.clear();
  next_row_id_ = maximum_row_id + 1;
  return true;
}

bool Database::recover(const uint8_t *wal, size_t size, RecoveryReport &report,
                       Error &error) {
  auto records = WalCodec(limits_).decode(wal, size, error);
  if (!records)
    return false;
  auto recovered = RecoveryManager(limits_).recover(pager_, *records, error);
  if (!recovered)
    return false;
  report = std::move(*recovered);
  return true;
}
bool Database::checkpoint(Error &error) {
  error.clear();
  transaction_.clear_wal();
  return true;
}
uint64_t Database::invariant_hash() const {
  uint64_t hash = 1469598103934665603ull;
  auto absorb = [&](std::string_view value) {
    for (unsigned char byte : value) {
      hash ^= byte;
      hash *= 1099511628211ull;
    }
  };
  for (const auto &table : catalog_.tables()) {
    absorb(table.first);
    for (const RowRecord &row : rows_.at(table.first)) {
      hash ^= row.row_id;
      hash *= 1099511628211ull;
      hash ^= row.generation;
      for (const Value &value : row.values)
        absorb(value.display());
    }
  }
  return hash;
}
std::vector<std::string> Database::verify() const {
  std::vector<std::string> issues;
  std::set<uint64_t> row_ids;
  for (const auto &table : catalog_.tables()) {
    auto found = rows_.find(table.first);
    if (found == rows_.end()) {
      issues.push_back("catalog table has no row storage");
      continue;
    }
    for (const RowRecord &row : found->second) {
      if (!row_ids.insert(row.row_id).second)
        issues.push_back("row identifier is duplicated");
      if (row.values.size() != table.second.columns.size())
        issues.push_back("row width differs from schema");
      for (size_t i = 0;
           i < row.values.size() && i < table.second.columns.size(); ++i)
        if (!compatible(row.values[i], table.second.columns[i]))
          issues.push_back("row value violates column type or nullability");
    }
  }
  for (const auto &index : indexes_) {
    Error error;
    if (!index.second.verify(error))
      issues.push_back("index invariant failed: " + error.message);
  }
  return issues;
}

} // namespace queryforge
