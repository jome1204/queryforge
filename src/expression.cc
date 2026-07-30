#include "internal.h"

namespace queryforge {
namespace {
std::optional<size_t> resolve(const Expression &expression,
                              const std::vector<Binding> &bindings,
                              Error &error) {
  std::string name = internal::normalize(expression.name);
  std::string qualifier = internal::normalize(expression.qualifier);
  std::optional<size_t> result;
  for (const Binding &binding : bindings) {
    if (internal::normalize(binding.name) != name)
      continue;
    if (!qualifier.empty() &&
        internal::normalize(binding.qualifier) != qualifier)
      continue;
    if (result) {
      internal::fail(error, ErrorCode::semantic_error, expression.offset,
                     "column reference is ambiguous");
      return std::nullopt;
    }
    result = binding.row_index;
  }
  if (!result)
    internal::fail(error, ErrorCode::not_found, expression.offset,
                   "column reference does not exist");
  return result;
}

bool like_match(std::string_view value, std::string_view pattern,
                size_t value_position = 0, size_t pattern_position = 0,
                uint32_t depth = 0) {
  if (depth > 128)
    return false;
  while (pattern_position < pattern.size()) {
    char p = pattern[pattern_position++];
    if (p == '%') {
      while (pattern_position < pattern.size() &&
             pattern[pattern_position] == '%')
        ++pattern_position;
      if (pattern_position == pattern.size())
        return true;
      for (size_t candidate = value_position; candidate <= value.size();
           ++candidate)
        if (like_match(value, pattern, candidate, pattern_position, depth + 1))
          return true;
      return false;
    }
    if (value_position >= value.size())
      return false;
    if (p != '_' && p != value[value_position])
      return false;
    ++value_position;
  }
  return value_position == value.size();
}

Value boolean_value(std::optional<bool> value) {
  return value ? Value(*value) : Value();
}
} // namespace

SemanticAnalyzer::SemanticAnalyzer(Limits limits) : limits_(limits) {}

std::optional<DataType>
SemanticAnalyzer::expression_type(const Expression &expression,
                                  const std::vector<Binding> &bindings,
                                  Error &error) const {
  if (expression.kind == Expression::Kind::literal)
    return expression.literal.type();
  if (expression.kind == Expression::Kind::column) {
    auto index = resolve(expression, bindings, error);
    if (!index)
      return std::nullopt;
    for (const Binding &binding : bindings)
      if (binding.row_index == *index)
        return binding.type;
    return std::nullopt;
  }
  if (expression.kind == Expression::Kind::unary) {
    if (!expression.left)
      return internal::fail(error, ErrorCode::semantic_error, expression.offset,
                            "unary expression has no child"),
             std::nullopt;
    auto child = expression_type(*expression.left, bindings, error);
    if (!child)
      return std::nullopt;
    if (expression.unary == UnaryOperator::is_null ||
        expression.unary == UnaryOperator::is_not_null ||
        expression.unary == UnaryOperator::logical_not)
      return DataType::boolean;
    if (*child != DataType::integer && *child != DataType::real &&
        *child != DataType::null_type) {
      internal::fail(error, ErrorCode::type_error, expression.offset,
                     "numeric unary operator requires number");
      return std::nullopt;
    }
    return *child;
  }
  if (expression.kind == Expression::Kind::binary) {
    if (!expression.left || !expression.right) {
      internal::fail(error, ErrorCode::semantic_error, expression.offset,
                     "binary expression has missing child");
      return std::nullopt;
    }
    auto left = expression_type(*expression.left, bindings, error);
    auto right = expression_type(*expression.right, bindings, error);
    if (!left || !right)
      return std::nullopt;
    switch (expression.binary) {
    case BinaryOperator::add:
    case BinaryOperator::subtract:
    case BinaryOperator::multiply:
    case BinaryOperator::divide:
    case BinaryOperator::modulo:
      if ((*left != DataType::integer && *left != DataType::real &&
           *left != DataType::null_type) ||
          (*right != DataType::integer && *right != DataType::real &&
           *right != DataType::null_type)) {
        internal::fail(error, ErrorCode::type_error, expression.offset,
                       "arithmetic operator requires numeric operands");
        return std::nullopt;
      }
      return (*left == DataType::real || *right == DataType::real)
                 ? DataType::real
                 : DataType::integer;
    default:
      return DataType::boolean;
    }
  }
  if (expression.kind == Expression::Kind::function) {
    std::string name = internal::normalize(expression.name);
    if (name == "COUNT")
      return DataType::integer;
    if (name == "SUM" || name == "AVG")
      return DataType::real;
    if (name == "MIN" || name == "MAX") {
      if (expression.arguments.size() != 1) {
        internal::fail(error, ErrorCode::semantic_error, expression.offset,
                       "MIN and MAX require one argument");
        return std::nullopt;
      }
      return expression_type(*expression.arguments[0], bindings, error);
    }
    if (name == "LENGTH")
      return DataType::integer;
    if (name == "LOWER" || name == "UPPER")
      return DataType::text;
    if (name == "COALESCE") {
      if (expression.arguments.empty()) {
        internal::fail(error, ErrorCode::semantic_error, expression.offset,
                       "COALESCE requires arguments");
        return std::nullopt;
      }
      return expression_type(*expression.arguments[0], bindings, error);
    }
    internal::fail(error, ErrorCode::unsupported, expression.offset,
                   "unknown function");
    return std::nullopt;
  }
  internal::fail(error, ErrorCode::unsupported, expression.offset,
                 "unsupported expression kind");
  return std::nullopt;
}

bool SemanticAnalyzer::analyze(const Statement &statement,
                               const Catalog &catalog, Error &error) const {
  error.clear();
  auto bindings_for = [&](std::string_view table, std::string_view alias) {
    std::vector<Binding> bindings;
    const Schema *schema = catalog.table(table);
    if (!schema)
      return bindings;
    for (size_t i = 0; i < schema->columns.size(); ++i)
      bindings.push_back(
          {alias.empty() ? std::string(table) : std::string(alias),
           schema->columns[i].name, schema->columns[i].type, i});
    return bindings;
  };
  if (auto select = std::get_if<SelectStatement>(&statement.data)) {
    std::vector<Binding> bindings;
    if (!select->table.empty()) {
      const Schema *schema = catalog.table(select->table);
      if (!schema)
        return internal::fail(error, ErrorCode::not_found, statement.offset,
                              "SELECT table does not exist");
      bindings = bindings_for(select->table, select->alias);
      size_t row_index = bindings.size();
      for (const JoinClause &join : select->joins) {
        const Schema *joined = catalog.table(join.table);
        if (!joined)
          return internal::fail(error, ErrorCode::not_found, statement.offset,
                                "joined table does not exist");
        for (const Column &column : joined->columns)
          bindings.push_back({join.alias.empty() ? join.table : join.alias,
                              column.name, column.type, row_index++});
        auto type = expression_type(*join.condition, bindings, error);
        if (!type)
          return false;
      }
    }
    for (const SelectItem &item : select->items)
      if (!item.wildcard && !expression_type(*item.expression, bindings, error))
        return false;
    if (select->where && !expression_type(*select->where, bindings, error))
      return false;
    return true;
  }
  auto table_name = std::visit(
      [](const auto &value) -> std::string {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, InsertStatement> ||
                      std::is_same_v<T, UpdateStatement> ||
                      std::is_same_v<T, DeleteStatement>)
          return value.table;
        return {};
      },
      statement.data);
  if (!table_name.empty() && !catalog.table(table_name))
    return internal::fail(error, ErrorCode::not_found, statement.offset,
                          "statement table does not exist");
  return true;
}

ExpressionEvaluator::ExpressionEvaluator(Limits limits) : limits_(limits) {}

std::optional<Value>
ExpressionEvaluator::evaluate(const Expression &expression, const Row &row,
                              const std::vector<Binding> &bindings,
                              Error &error) const {
  error.clear();
  if (expression.kind == Expression::Kind::literal)
    return expression.literal;
  if (expression.kind == Expression::Kind::column) {
    auto index = resolve(expression, bindings, error);
    if (!index || *index >= row.size()) {
      if (!error)
        internal::fail(error, ErrorCode::invalid_offset, expression.offset,
                       "column binding exceeds row");
      return std::nullopt;
    }
    return row[*index];
  }
  if (expression.kind == Expression::Kind::unary) {
    if (!expression.left) {
      internal::fail(error, ErrorCode::semantic_error, expression.offset,
                     "unary expression child missing");
      return std::nullopt;
    }
    auto value = evaluate(*expression.left, row, bindings, error);
    if (!value)
      return std::nullopt;
    if (expression.unary == UnaryOperator::is_null)
      return Value(value->is_null());
    if (expression.unary == UnaryOperator::is_not_null)
      return Value(!value->is_null());
    if (value->is_null())
      return Value();
    if (expression.unary == UnaryOperator::logical_not) {
      auto boolean = value->as_boolean();
      return boolean ? std::optional<Value>(Value(!*boolean))
                     : std::optional<Value>(Value());
    }
    auto number = value->as_real();
    if (!number) {
      internal::fail(error, ErrorCode::type_error, expression.offset,
                     "numeric unary operator requires number");
      return std::nullopt;
    }
    if (expression.unary == UnaryOperator::positive)
      return *value;
    if (value->type() == DataType::integer) {
      auto integer = value->as_integer();
      if (*integer == std::numeric_limits<int64_t>::min()) {
        internal::fail(error, ErrorCode::overflow, expression.offset,
                       "integer negation overflows");
        return std::nullopt;
      }
      return Value(-*integer);
    }
    return Value(-*number);
  }
  if (expression.kind == Expression::Kind::binary) {
    auto left = evaluate(*expression.left, row, bindings, error);
    auto right = evaluate(*expression.right, row, bindings, error);
    if (!left || !right)
      return std::nullopt;
    if (expression.binary == BinaryOperator::logical_and ||
        expression.binary == BinaryOperator::logical_or) {
      auto a = left->as_boolean();
      auto b = right->as_boolean();
      if (expression.binary == BinaryOperator::logical_and) {
        if ((a && !*a) || (b && !*b))
          return Value(false);
        return a && b ? Value(*a && *b) : Value();
      }
      if ((a && *a) || (b && *b))
        return Value(true);
      return a && b ? Value(*a || *b) : Value();
    }
    if (left->is_null() || right->is_null())
      return Value();
    if (expression.binary >= BinaryOperator::equal &&
        expression.binary <= BinaryOperator::greater_equal) {
      CompareResult comparison = compare_values(*left, *right);
      if (comparison == CompareResult::unordered)
        return Value();
      switch (expression.binary) {
      case BinaryOperator::equal:
        return Value(comparison == CompareResult::equal);
      case BinaryOperator::not_equal:
        return Value(comparison != CompareResult::equal);
      case BinaryOperator::less:
        return Value(comparison == CompareResult::less);
      case BinaryOperator::less_equal:
        return Value(comparison != CompareResult::greater);
      case BinaryOperator::greater:
        return Value(comparison == CompareResult::greater);
      case BinaryOperator::greater_equal:
        return Value(comparison != CompareResult::less);
      default:
        break;
      }
    }
    if (expression.binary == BinaryOperator::like) {
      auto value = left->as_text();
      auto pattern = right->as_text();
      if (!value || !pattern)
        return internal::fail(error, ErrorCode::type_error, expression.offset,
                              "LIKE requires text operands"),
               std::nullopt;
      return Value(like_match(*value, *pattern));
    }
    auto a = left->as_real();
    auto b = right->as_real();
    if (!a || !b)
      return internal::fail(error, ErrorCode::type_error, expression.offset,
                            "arithmetic requires numeric operands"),
             std::nullopt;
    switch (expression.binary) {
    case BinaryOperator::add:
      return Value(*a + *b);
    case BinaryOperator::subtract:
      return Value(*a - *b);
    case BinaryOperator::multiply:
      return Value(*a * *b);
    case BinaryOperator::divide:
      if (*b == 0.0)
        return Value();
      return Value(*a / *b);
    case BinaryOperator::modulo: {
      auto ai = left->as_integer();
      auto bi = right->as_integer();
      if (!ai || !bi || *bi == 0)
        return Value();
      if (*ai == std::numeric_limits<int64_t>::min() && *bi == -1)
        return Value(int64_t{0});
      return Value(*ai % *bi);
    }
    default:
      break;
    }
  }
  if (expression.kind == Expression::Kind::function) {
    std::string name = internal::normalize(expression.name);
    if (name == "COALESCE") {
      for (const auto &argument : expression.arguments) {
        auto value = evaluate(*argument, row, bindings, error);
        if (!value)
          return std::nullopt;
        if (!value->is_null())
          return value;
      }
      return Value();
    }
    if (expression.arguments.size() != 1)
      return internal::fail(error, ErrorCode::semantic_error, expression.offset,
                            "scalar function requires one argument"),
             std::nullopt;
    auto value = evaluate(*expression.arguments[0], row, bindings, error);
    if (!value)
      return std::nullopt;
    if (name == "LENGTH") {
      if (auto text = value->as_text())
        return Value(static_cast<int64_t>(text->size()));
      if (auto blob = value->as_blob())
        return Value(static_cast<int64_t>(blob->size()));
      return Value();
    }
    if (name == "LOWER" || name == "UPPER") {
      auto text = value->as_text();
      if (!text)
        return Value();
      std::string converted(*text);
      for (char &c : converted)
        c = static_cast<char>(
            name == "LOWER" ? std::tolower(static_cast<unsigned char>(c))
                            : std::toupper(static_cast<unsigned char>(c)));
      return Value(std::move(converted));
    }
  }
  internal::fail(error, ErrorCode::unsupported, expression.offset,
                 "expression cannot be evaluated in row context");
  return std::nullopt;
}

std::optional<bool>
ExpressionEvaluator::predicate(const Expression &expression, const Row &row,
                               const std::vector<Binding> &bindings,
                               Error &error) const {
  auto value = evaluate(expression, row, bindings, error);
  if (!value)
    return std::nullopt;
  auto boolean = value->as_boolean();
  return boolean.value_or(false);
}

} // namespace queryforge
